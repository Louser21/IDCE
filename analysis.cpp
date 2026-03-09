#include "ir.h"
#include <iostream>
#include <regex>
#include <set>
#include <algorithm>
#include <queue>
#include <map>

std::string sanitizeVar(std::string var) {
    var.erase(std::remove_if(var.begin(), var.end(), [](char c) {
        return !(isalnum(c) || c == '_' || c == '.');
    }), var.end());
    return var;
}

bool hasSideEffect(const Statement& stmt) {
    if (stmt.type == STMT_CALL) {
        std::regex sys_call(R"(\b(printf|scanf|malloc|free|fopen|fclose|fwrite|fread|sys_.*)\b)");
        if (std::regex_search(stmt.text, sys_call)) return true;
    }
    if (stmt.text.find("*") != std::string::npos && stmt.text.find("=") != std::string::npos) {
        std::string lhs = extractLHS(stmt.text);
        if (lhs.find("*") != std::string::npos) return true;
    }
    return false;
}

std::string extractLHS(const std::string &text) {
    std::regex lhs_regex(R"(([a-zA-Z0-9_._]*)\s*=)");
    std::smatch match;
    if (std::regex_search(text, match, lhs_regex))
        return match[1].str();
    return "";
}

std::set<std::string> extractRHS(const std::string &text) {
    std::set<std::string> uses;
    std::regex var_regex(R"(([a-zA-Z_][a-zA-Z0-9_.]*))");
    std::string lhs = extractLHS(text);

    auto words_begin = std::sregex_iterator(text.begin(), text.end(), var_regex);
    auto words_end   = std::sregex_iterator();

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

void applyConstantFolding(FunctionIR &func) {
    std::map<std::string, int> constants;
    std::set<int> executable_edges;
    bool changed = true;

    std::regex const_assign_regex(R"(([a-zA-Z0-9_._]+)\s*=\s*([0-9]+))");
    std::regex var_assign_regex(R"(([a-zA-Z0-9_._]+)\s*=\s*([a-zA-Z0-9_._]+))");
    std::regex math_op_regex(R"(([a-zA-Z0-9_._]+)\s*=\s*([a-zA-Z0-9_._]+)\s*([\+\-\*\/])\s*([a-zA-Z0-9_._]+))");
    std::regex if_cond_regex(R"(if\s*\((.+)\s*([<>!=]+)\s*([0-9a-zA-Z_._]+)\))");

    while (changed) {
        changed = false;
        for (auto &block : func.blocks) {
            for (auto &stmt : block.statements) {
                if (stmt.text.find("// Folded") != std::string::npos) continue;

                std::smatch match;
                if (std::regex_search(stmt.text, match, const_assign_regex)) {
                    std::string var = sanitizeVar(match[1].str());
                    int val = std::stoi(match[2].str());
                    if (constants.find(var) == constants.end() || constants[var] != val) {
                        constants[var] = val;
                        changed = true;
                    }
                } else if (std::regex_search(stmt.text, match, var_assign_regex) && stmt.text.find("+") == std::string::npos && stmt.text.find("-") == std::string::npos) {
                    std::string var = sanitizeVar(match[1].str());
                    std::string src = sanitizeVar(match[2].str());
                    if (constants.count(src) && (!constants.count(var) || constants[var] != constants[src])) {
                        constants[var] = constants[src];
                        changed = true;
                    }
                } else if (std::regex_search(stmt.text, match, math_op_regex)) {
                    std::string var  = sanitizeVar(match[1].str());
                    std::string op1  = sanitizeVar(match[2].str());
                    std::string oper = match[3].str();
                    std::string op2  = sanitizeVar(match[4].str());

                    int v1 = 0, v2 = 0;
                    bool has_v1 = constants.count(op1);
                    bool has_v2 = constants.count(op2);

                    if (!has_v1 && isdigit(op1[0])) { v1 = std::stoi(op1); has_v1 = true; }
                    else if (has_v1) v1 = constants[op1];
                    if (!has_v2 && isdigit(op2[0])) { v2 = std::stoi(op2); has_v2 = true; }
                    else if (has_v2) v2 = constants[op2];

                    if (has_v1 && has_v2) {
                        int r = 0;
                        if (oper == "+") r = v1 + v2;
                        else if (oper == "-") r = v1 - v2;
                        else if (oper == "*") r = v1 * v2;
                        else if (oper == "/") { if (v2 != 0) r = v1 / v2; else continue; }

                        if (constants.find(var) == constants.end() || constants[var] != r) {
                            constants[var] = r;
                            std::cout << "[DCE Log] SCCP Evaluated Math: " << var << " = " << r << "\n";
                            changed = true;
                        }
                    }
                } else if (stmt.type == STMT_COND && std::regex_search(stmt.text, match, if_cond_regex)) {
                    std::string var        = sanitizeVar(match[1].str());
                    std::string op         = match[2].str();
                    std::string thresh_str = sanitizeVar(match[3].str());

                    int threshold = 0;
                    if (constants.count(thresh_str)) threshold = constants[thresh_str];
                    else if (isdigit(thresh_str[0])) threshold = std::stoi(thresh_str);
                    else continue;

                    if (constants.count(var)) {
                        int actual_val = constants[var];
                        bool is_true = false;
                        if (op == "==") is_true = (actual_val == threshold);
                        else if (op == "!=") is_true = (actual_val != threshold);
                        else if (op == ">")  is_true = (actual_val > threshold);
                        else if (op == "<")  is_true = (actual_val < threshold);

                        if (!is_true) {
                            std::cout << "[DCE Log] SCCP Folding branch to FALSE: " << stmt.text << "\n";
                            stmt.text = "// Folded: " + stmt.text;
                            stmt.type = STMT_OTHER;
                            block.successors.clear();
                            changed = true;
                        }
                    }
                }
            }
        }
    }
}

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

void applyDCE(FunctionIR &func) {
    applyConstantFolding(func);
    removeUnreachableBlocks(func);

    std::set<int> live_stmt_ids;
    std::set<std::string> live_vars;
    bool changed = true;

    auto mark_live = [&](const Statement &stmt) {
        if (stmt.type == STMT_RETURN || stmt.type == STMT_CALL ||
            stmt.type == STMT_GOTO   || stmt.type == STMT_COND || hasSideEffect(stmt)) {
            live_stmt_ids.insert(stmt.id);
            auto uses = extractRHS(stmt.text);
            live_vars.insert(uses.begin(), uses.end());
        }
    };

    for (const auto &s : func.preamble) mark_live(s);
    for (auto &b : func.blocks) for (const auto &s : b.statements) mark_live(s);

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
                if (!hasSideEffect(s)) {
                    std::cout << "[DCE Log] Removing dead entry (DSE/Liveness): " << s.text << "\n";
                    return true;
                }
            }
            return false;
        }), stmts.end());
    };

    sweep(func.preamble);
    for (auto &b : func.blocks) sweep(b.statements);
}

void applyGlobalDCE(ProgramIR &prog) {
    removeUnusedFunctions(prog);
    for (auto &func : prog.functions) {
        applyDCE(func);
    }
}

void applyIntelligentDCE(ProgramIR &prog, const std::set<int>& dead_ids) {
    auto sweep = [&](std::vector<Statement> &stmts) {
        stmts.erase(std::remove_if(stmts.begin(), stmts.end(), [&](const Statement &s) {
            if (dead_ids.find(s.id) != dead_ids.end()) {
                if (!hasSideEffect(s)) {
                    std::cout << "[ML DCE Log] Safely Removing predicted dead entry: " << s.text << "\n";
                    return true;
                } else {
                    std::cout << "[ML DCE Warning] Vetoed ML prediction due to side effect: " << s.text << "\n";
                }
            }
            return false;
        }), stmts.end());
    };

    for (auto &func : prog.functions) {
        sweep(func.preamble);
        for (auto &b : func.blocks) {
            sweep(b.statements);
        }
    }
}