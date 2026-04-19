#!/usr/bin/env python3
import sys
import json
import os
import re

def run_inference(json_path):
    try:
        if not os.path.exists(json_path): return
        with open(json_path, 'r') as f:
            data = json.load(f)

        dead_items = []
        all_nodes = {}
        var_uses = {}
        dead_map = {}

        for func in data.get('functions', []):
            for node in func.get('nodes', []):
                all_nodes[node['id']] = node
                for var in node.get('rhs', []):
                    if var not in var_uses: var_uses[var] = set()
                    var_uses[var].add(node['id'])

        # 1. Function Reachability (DFE)
        reachable_funcs = set()
        queue = ["main"]
        reachable_funcs.add("main")
        processed_funcs = set()
        while queue:
            curr = queue.pop(0)
            if curr in processed_funcs: continue
            processed_funcs.add(curr)
            target_func = next((f for f in data.get('functions', []) if curr in f['name']), None)
            if target_func:
                reachable_funcs.add(target_func['name'])
                for node in target_func.get('nodes', []):
                    for match in re.findall(r'([a-zA-Z0-9_._]+)\s*\(', node['text']):
                        if match not in reachable_funcs:
                            queue.append(match)

        for func in data.get('functions', []):
            if not any(r in func['name'] for r in reachable_funcs):
                for node in func.get('nodes', []):
                    dead_map[node['id']] = "unused function"

        # 2. Pattern Detection (Iterative)
        changed = True
        while changed:
            changed = False
            for func in data.get('functions', []):
                func_nodes = [n for n in func.get('nodes', []) if n['id'] not in dead_map]
                blocks = {}
                for n in func['nodes']:
                    bid = n['block_id']
                    if bid not in blocks: blocks[bid] = []
                    blocks[bid].append(n)

                for node in func_nodes:
                    f = node['features']
                    opcode, side_effect, after_terminal = f[0], f[1], f[2]
                    text = node['text'].lower()
                    stmt_id = node['id']
                    lhs = node.get('lhs', '')

                    reason = None
                    if after_terminal:
                        reason = "unreachable statement"
                    elif opcode == 4: # COND
                         # Structural: Redundant branch (both branches do same thing)
                         # This needs edge info from the graph.
                         if any(x in text for x in ["0 == 1", "0 != 0", "false", "(0)"]):
                            reason = "constant false condition"
                    elif opcode == 1: # ASSIGN
                        parts = text.split('=')
                        if len(parts) == 2 and parts[0].strip() == parts[1].strip().replace(';',''):
                            reason = "no-op statement"
                        elif lhs:
                            actual_uses = [uid for uid in var_uses.get(lhs, []) if uid not in dead_map]
                            if not actual_uses and not side_effect:
                                reason = "dead store"
                    elif opcode == 2 and not side_effect:
                         if "std::" not in text and "(" in text:
                            reason = "redundant computation"
                    elif any(x in text for x in [" + 0", " * 1", " - 0"]):
                         reason = "redundant computation"
                    
                    if reason:
                        dead_map[stmt_id] = reason
                        changed = True

        for sid, reason in dead_map.items():
            dead_items.append({"id": sid, "reason": reason})
        print(json.dumps({"dead_items": dead_items}))
    except Exception as e:
        print(json.dumps({"dead_items": [], "error": str(e)}))

if __name__ == "__main__":
    if len(sys.argv) < 2: sys.exit(1)
    run_inference(sys.argv[1])
