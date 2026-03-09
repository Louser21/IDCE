# IDCE — Setup Guide

Everything below is what you need to run after cloning the project on a new machine.

---

## 1. Clone the repo

```bash
git clone https://github.com/Louser21/IDCE.git
cd IDCE
```

---

## 2. System dependencies

```bash
sudo apt update
sudo apt install -y g++ flex bison graphviz
```

| Package | Used for |
|---------|----------|
| `g++` | Compiling the C++ compiler backend |
| `flex` | Generating the lexer from `lexer.l` |
| `bison` | Generating the parser from `parser.y` |
| `graphviz` | Rendering `.dot` CFG files to PNG (`dot` command) |

---

## 3. Build the compiler binary

```bash
make
```

This produces the `idce` binary. To verify:

```bash
./idce analysis_report < inputs/input.ssa
```

---

## 4. Python ML environment

```bash
cd ml
python3 -m venv .venv
source .venv/bin/activate
pip install torch torchvision torchaudio --index-url https://download.pytorch.org/whl/cpu
pip install torch_geometric
```

> **GPU (optional):** Replace the torch install URL with your CUDA version.  
> Check: https://pytorch.org/get-started/locally/

To verify:

```bash
python3 -c "import torch; from torch_geometric.nn import SAGEConv; print('ML ready')"
```

---

## 5. Generate training data & train the model

> Skip this if you already have `ml_data/dce_model.pt` from the repo.

```bash
# From the project root (not inside ml/)
source ml/.venv/bin/activate

# Generate 100 synthetic SSA programs
python3 ml/fuzzer.py

# Convert them into a PyTorch Geometric dataset
python3 ml/dataset_gen.py

# Train the GNN (40 epochs, ~1–2 min on CPU)
cd ml
python3 train.py
cd ..
```

Trained weights are saved to `ml_data/dce_model.pt`.

---

## 6. GUI (web frontend)

```bash
cd gui
npm install
npm start
```

Open **http://localhost:3000** in your browser.

> To stop the server, press `Ctrl+C`.  
> To restart: `npm start` (automatically clears port 3000).

---

## Quick reference

| Task | Command |
|------|---------|
| Build binary | `make` |
| Clean build artifacts | `make clean` |
| Run classical DCE | `./idce <output_dir> < input.ssa` |
| Run ML-guided DCE | `./idce <output_dir> --ml-dce < input.ssa` |
| Export IR graph (for dataset) | `./idce <output_dir> --ml-extract < input.ssa` |
| Render CFG image | `dot -Tpng <output_dir>/graph.dot -o graph.png` |
| Generate training data | `python3 ml/fuzzer.py && python3 ml/dataset_gen.py` |
| Train model | `cd ml && python3 train.py` |
| Start GUI | `cd gui && npm start` |

---

## What gets regenerated automatically

These files/folders are **not in the repo** and are created when you run the commands above:

| Path | Created by |
|------|-----------|
| `idce` | `make` |
| `lex.yy.c`, `parser.tab.*` | `make` (flex/bison) |
| `ml/.venv/` | `python3 -m venv .venv` |
| `ml_data/raw/` | `python3 ml/fuzzer.py` |
| `ml_data/processed/` | `python3 ml/dataset_gen.py` |
| `gui/node_modules/` | `npm install` |
| `analysis_report/`, `out_*/` | Running `./idce` |
