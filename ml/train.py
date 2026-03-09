import torch
import torch.nn.functional as F
from torch_geometric.loader import DataLoader
from model import DCENodeClassifier
import random

def train():
    print("Loading Dataset...")
    dataset = torch.load("ml_data/processed/dataset.pt", weights_only=False)
    random.shuffle(dataset)

    train_size = int(len(dataset) * 0.8)
    train_dataset = dataset[:train_size]
    test_dataset = dataset[train_size:]

    train_loader = DataLoader(train_dataset, batch_size=32, shuffle=True)
    test_loader = DataLoader(test_dataset, batch_size=32, shuffle=False)

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"Using device: {device}")

    model = DCENodeClassifier(in_channels=2, hidden_channels=32).to(device)
    optimizer = torch.optim.Adam(model.parameters(), lr=0.01)
    criterion = torch.nn.BCEWithLogitsLoss(pos_weight=torch.tensor([0.2]).to(device))

    epochs = 40
    for epoch in range(1, epochs + 1):
        model.train()
        total_loss = 0
        for data in train_loader:
            data = data.to(device)
            optimizer.zero_grad()
            out = model(data.x, data.edge_index)
            loss = criterion(out, data.y.float())
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * data.num_graphs

        train_loss = total_loss / len(train_dataset)

        model.eval()
        tp = fp = fn = tn = 0
        with torch.no_grad():
            for data in test_loader:
                data = data.to(device)
                out = model(data.x, data.edge_index)
                preds = (torch.sigmoid(out) > 0.5).long()

                tp += ((preds == 1) & (data.y == 1)).sum().item()
                fp += ((preds == 1) & (data.y == 0)).sum().item()
                fn += ((preds == 0) & (data.y == 1)).sum().item()
                tn += ((preds == 0) & (data.y == 0)).sum().item()

        precision = tp / (tp + fp) if (tp + fp) > 0 else 0
        recall = tp / (tp + fn) if (tp + fn) > 0 else 0

        if epoch % 5 == 0:
            print(f"Epoch {epoch:03d}: Loss: {train_loss:.4f} | Test Precision: {precision:.4f} | Test Recall: {recall:.4f}")

    print("Training Complete. Exporting Weights.")
    torch.save(model.state_dict(), "ml_data/dce_model.pt")

if __name__ == "__main__":
    train()
