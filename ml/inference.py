#!/usr/bin/env python3
import sys
import json
import torch
from torch_geometric.data import Data
import os

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from model import DCENodeClassifier

def run_inference(json_path, model_path):
    with open(json_path, 'r') as f:
        data = json.load(f)

    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')

    model = DCENodeClassifier(in_channels=2, hidden_channels=32).to(device)
    model.load_state_dict(torch.load(model_path, map_location=device, weights_only=True))
    model.eval()

    dead_ids = []

    with torch.no_grad():
        for func in data.get('functions', []):
            if not func['nodes']: continue

            id_to_idx = {node['id']: i for i, node in enumerate(func['nodes'])}
            idx_to_id = {i: node['id'] for i, node in enumerate(func['nodes'])}

            x = [node['features'] for node in func['nodes']]
            x_tensor = torch.tensor(x, dtype=torch.float)

            edge_index = [[], []]
            for edge in func.get('edges', []):
                src = id_to_idx.get(edge['source'])
                tgt = id_to_idx.get(edge['target'])
                if src is not None and tgt is not None:
                    edge_index[0].append(src)
                    edge_index[1].append(tgt)

            edge_tensor = torch.tensor(edge_index, dtype=torch.long)

            x_tensor = x_tensor.to(device)
            edge_tensor = edge_tensor.to(device)

            out = model(x_tensor, edge_tensor)
            preds = (torch.sigmoid(out) > 0.5).long()

            for i, p in enumerate(preds):
                if p.item() == 1:
                    dead_ids.append(idx_to_id[i])

    print(json.dumps({"dead_ids": dead_ids}))

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 inference.py <graph.json> <model.pt>", file=sys.stderr)
        sys.exit(1)

    run_inference(sys.argv[1], sys.argv[2])
