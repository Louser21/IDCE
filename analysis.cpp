#include "ir.h"
#include <iostream>
#include <regex>
#include <set>
#include <algorithm>
#include <queue>
#include <map>

// Helper to clean SSA variable names for reliable map lookups
std::string sanitizeVar(std::string var) {
    var.erase(std::remove_if(var.begin(), var.end(), [](char c) {
        return !(isalnum(c) || c == '_' || c == '.');
    }), var.end());
    return var;
}

// Helper: Extract LHS variable for statement analysis
std::string extractLHS(const std::string &text) {
    std::regex lhs_regex(R"(([a-zA-Z0-9_._]*)\s*=)");
    std::smatch match;
    if (std::regex_search(text, match, lhs_regex))
        return match[1].str();
    return "";
}

// Helper: Extract RHS variables, handling SSA versions like x_1 or temp.1
std::set<std::string> extractRHS(const std::string &text) {
    std::set<std::string> uses;
    std::regex var_regex(R"(([a-zA-Z_][a-zA-Z0-9_.]*))");
    std::string lhs = extractLHS(text);

    auto words_begin = std::sregex_iterator(text.begin(), text.end(), var_regex);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::string var = i->str();
        if (var == lhs) continue;
        if (var == "int" || var == "float" || var == "return" || var == "goto" || var == "if") continue;
        if (isdigit(var[0])) continue;
        if (var.find("bb") != std::string::npos) continue;

        uses.insert(var);
    }
    return uses;
}

// Phase A: Constant Folding to track and mask unreachable conditions
void applyConstantFolding(FunctionIR &func) {
    std::map<std::string, int> constants;
    std::regex const_assign_regex(R"(([a-zA-Z0-9_._]+)\s*=\s*([0-9]+))");
    std::regex if_cond_regex(R"(if\s*\((.+)\s*([<>!=]+)\s*([0-9]+)\))");

    for (auto &block : func.blocks) {
        for (auto &stmt : block.statements) {
            std::smatch match;
            if (std::regex_search(stmt.text, match, const_assign_regex)) {
                std::string var = sanitizeVar(match[1].str());
                constants[var] = std::stoi(match[2].str());
                std::cout << "[DCE Debug] Tracked constant: [" << var << "] = " << constants[var] << "\n";
            }

            if (stmt.type == STMT_COND && std::regex_search(stmt.text, match, if_cond_regex)) {
                std::string var = sanitizeVar(match[1].str());
                std::string op = match[2].str();
                int threshold = std::stoi(match[3].str());

                if (constants.count(var)) {
                    int actual_val = constants[var];
                    bool is_true = false;
                    if (op == "==") is_true = (actual_val == threshold);
                    else if (op == "!=") is_true = (actual_val != threshold);
                    else if (op == ">")  is_true = (actual_val > threshold);
                    else if (op == "<")  is_true = (actual_val < threshold);

                    if (!is_true) {
                        std::cout << "[DCE Log] Folding constant condition: " << stmt.text << " to FALSE\n";
                        stmt.text = "// Folded: " + stmt.text;
                        stmt.type = STMT_OTHER; 
                        block.successors.clear(); // Prune the CFG edge
                    }
                }
            }
        }
    }
}

// Phase B: Reachability Analysis (Structural DCE) to remove unreachable blocks
void removeUnreachableBlocks(FunctionIR &func) {
    if (func.blocks.empty()) return;

    std::set<int> reachable_ids;
    std::queue<int> worklist;

    int entry_id = func.blocks[0].id;
    worklist.push(entry_id);
    reachable_ids.insert(entry_id);

    while (!worklist.empty()) {
        int current_id = worklist.front();
        worklist.pop();
        for (const auto &block : func.blocks) {
            if (block.id == current_id) {
                for (int succ : block.successors) {
                    if (reachable_ids.find(succ) == reachable_ids.end()) {
                        reachable_ids.insert(succ);
                        worklist.push(succ);
                    }
                }
                break;
            }
        }
    }

    func.blocks.erase(std::remove_if(func.blocks.begin(), func.blocks.end(),
        [&](const BasicBlock &b) {
            if (reachable_ids.find(b.id) == reachable_ids.end()) {
                std::cout << "[DCE Log] Removing unreachable Block " << b.id << std::endl;
                return true;
            }
            return false;
        }), func.blocks.end());
}

// Phase C: Global Pruning to remove functions never called (Inter-procedural)
void removeUnusedFunctions(ProgramIR &prog) {
    std::set<std::string> called_functions;
    called_functions.insert("main");

    for (const auto &func : prog.functions) {
        for (const auto &block : func.blocks) {
            for (const auto &stmt : block.statements) {
                if (stmt.type == STMT_CALL) {
                    std::regex call_regex(R"(([a-zA-Z0-9_._]+)\s*\()");
                    std::smatch match;
                    if (std::regex_search(stmt.text, match, call_regex)) {
                        called_functions.insert(match[1].str());
                    }
                }
            }
        }
    }

    prog.functions.erase(std::remove_if(prog.functions.begin(), prog.functions.end(),
        [&](const FunctionIR &f) {
            bool is_used = false;
            for (const auto &called : called_functions) {
                if (f.name.find(called) != std::string::npos) {
                    is_used = true;
                    break;
                }
            }
            if (!is_used) {
                std::cout << "[DCE Log] Removing unused function: " << f.name << "\n";
                return true;
            }
            return false;
        }), prog.functions.end());
}

// Phase D: Intra-procedural Variable Liveness (Mark-and-Sweep)
void applyDCE(FunctionIR &func) {
    // 1. First, process logic to break CFG edges
    applyConstantFolding(func);
    // 2. IMMEDIATELY remove orphaned blocks so liveness pass doesn't see them
    removeUnreachableBlocks(func);

    std::set<int> live_stmt_ids;
    std::set<std::string> live_vars;
    bool changed = true;

    auto mark_live = [&](const Statement &stmt) {
        if (stmt.type == STMT_RETURN || stmt.type == STMT_CALL ||
            stmt.type == STMT_GOTO || stmt.type == STMT_COND) {
            live_stmt_ids.insert(stmt.id);
            auto uses = extractRHS(stmt.text);
            live_vars.insert(uses.begin(), uses.end());
        }
    };

    for (const auto &s : func.preamble) mark_live(s);
    for (auto &b : func.blocks) for (const auto &s : b.statements) mark_live(s);

    // Fixed-point Propagation
    while (changed) {
        changed = false;
        for (auto &block : func.blocks) {
            for (auto it = block.statements.rbegin(); it != block.statements.rend(); ++it) {
                if (live_stmt_ids.count(it->id)) continue;
                std::string lhs = extractLHS(it->text);
                if (!lhs.empty() && live_vars.count(lhs)) {
                    live_stmt_ids.insert(it->id);
                    auto uses = extractRHS(it->text);
                    live_vars.insert(uses.begin(), uses.end());
                    changed = true;
                }
            }
        }
    }

    auto sweep = [&](std::vector<Statement> &stmts) {
        stmts.erase(std::remove_if(stmts.begin(), stmts.end(), [&](const Statement &s) {
            if (live_stmt_ids.find(s.id) == live_stmt_ids.end()) {
                if (s.type == STMT_ASSIGN || s.type == STMT_OTHER) {
                    std::cout << "[DCE Log] Removing dead entry: " << s.text << "\n";
                    return true;
                }
            }
            return false; 
        }), stmts.end());
    };

    sweep(func.preamble);
    for (auto &b : func.blocks) sweep(b.statements);
}

// Final Top-Level Entry Point: Combining Inter-procedural and Intra-procedural DCE
void applyGlobalDCE(ProgramIR &prog) {
    // Phase 1: Prune entire functions that aren't called (Inter-procedural)
    removeUnusedFunctions(prog); 

    // Phase 2: Perform deep cleaning on remaining functions
    for (auto &func : prog.functions) {
        applyDCE(func); 
    }
}