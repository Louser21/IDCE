#!/usr/bin/env python3
import json
import os
import subprocess
import torch
from torch_geometric.data import Data
import glob

RAW_DIR = "ml_data/raw"
OUT_DIR = "ml_data/processed"

def process_file(filepath):
    # Run the C++ compiler with ML feature extraction enabled
    # We output to a temporary specialized directory for this file
    base_name = os.path.basename(filepath).split('.')[0]
    out_folder = f"ml_data/tmp_{base_name}"
    
    cmd = f"./idce --ml-extract {out_folder} < {filepath}"
    try:
        subprocess.run(cmd, shell=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        return None # Skip on parser crash

    # Read the Unoptimized Feature Graph
    with open(f"{out_folder}/ir_graph_features.json", 'r') as f:
        unopt_data = json.load(f)

    # Read the Optimized Graph (Ground Truth for what survived DCE)
    with open(f"{out_folder}/ir_graph_optimized.json", 'r') as f:
        opt_data = json.load(f)

    # Build the Label Dictionary: Which IDs survived?
    survived_ids = set()
    for func in opt_data['functions']:
        for node in func['nodes']:
            survived_ids.add(node['id'])

    graphs = []
    # Convert each function into a PyTorch Geometric Data object
    for func in unopt_data['functions']:
        if not func['nodes']: continue

        # Map original IDs to continuous 0...N indices for PyTorch
        id_to_idx = {node['id']: i for i, node in enumerate(func['nodes'])}
        
        # 1. Node Features (Opcode, SideEffect)
        x = []
        y = []
        for node in func['nodes']:
            x.append(node['features'])
            # Label = 1 if Dead (not in survived_ids), else 0 if Alive
            is_dead = 1 if node['id'] not in survived_ids else 0
            y.append(is_dead)

        x_tensor = torch.tensor(x, dtype=torch.float)
        y_tensor = torch.tensor(y, dtype=torch.long)

        # 2. Edge Index (Source -> Target)
        edge_index = [[], []]
        for edge in func['edges']:
            src = id_to_idx.get(edge['source'])
            tgt = id_to_idx.get(edge['target'])
            if src is not None and tgt is not None:
                edge_index[0].append(src)
                edge_index[1].append(tgt)
        
        edge_tensor = torch.tensor(edge_index, dtype=torch.long)
        
        # Create Data object
        data = Data(x=x_tensor, edge_index=edge_tensor, y=y_tensor)
        graphs.append(data)

    # Cleanup temp folder
    subprocess.run(f"rm -rf {out_folder}", shell=True)
    return graphs

if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    all_graphs = []
    
    files = glob.glob(f"{RAW_DIR}/*.txt")
    print(f"Processing {len(files)} raw files into PyTorch Dataset...")
    
    for i, f in enumerate(files):
        graphs = process_file(f)
        if graphs:
            all_graphs.extend(graphs)
        if (i+1) % 20 == 0:
            print(f"Processed {i+1}/{len(files)} files...")
            
    # Save the entire dataset list
    torch.save(all_graphs, f"{OUT_DIR}/dataset.pt")
    print(f"Dataset generation complete! Generated {len(all_graphs)} graphs.")
    print(f"Saved to {OUT_DIR}/dataset.pt")
