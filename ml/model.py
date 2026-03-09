import torch
import torch.nn.functional as F
from torch_geometric.nn import SAGEConv

class DCENodeClassifier(torch.nn.Module):
    def __init__(self, in_channels, hidden_channels):
        super(DCENodeClassifier, self).__init__()
        self.conv1 = SAGEConv(in_channels, hidden_channels)
        self.conv2 = SAGEConv(hidden_channels, hidden_channels)
        self.classifier = torch.nn.Linear(hidden_channels, 1)

    def forward(self, x, edge_index):
        x = self.conv1(x, edge_index)
        x = F.relu(x)
        x = F.dropout(x, p=0.2, training=self.training)
        x = self.conv2(x, edge_index)
        x = F.relu(x)
        out = self.classifier(x).squeeze(-1)
        return out
