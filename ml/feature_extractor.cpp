#include "../ir.h"
#include <iostream>
#include <fstream>
#include <map>
#include <vector>
#include <set>
#include <string>

// External declaration for safety helper
extern bool hasSideEffect(const Statement& stmt);
extern std::string extractLHS(const std::string &text);
extern std::set<std::string> extractRHS(const std::string &text);

// Helper to escape JSON strings safely
std::string escapeJSON(const std::string& input) {
    std::string output;
    for (char c : input) {
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\n') output += "\\n";
        else output += c;
    }
    return output;
}

// Opcode basic tokenizer for ML embeddings
int getOpcodeEmbedding(StmtType type) {
    switch(type) {
        case STMT_ASSIGN: return 1;
        case STMT_CALL: return 2;
        case STMT_GOTO: return 3;
        case STMT_COND: return 4;
        case STMT_RETURN: return 5;
        default: return 0; // STMT_OTHER / Unrecognized
    }
}

void exportIRFeaturesToJSON(const ProgramIR& prog, const std::string& out_path) {
    std::ofstream out(out_path);
    out << "{\n  \"functions\": [\n";

    for (size_t f_idx = 0; f_idx < prog.functions.size(); ++f_idx) {
        const auto& func = prog.functions[f_idx];
        out << "    {\n";
        out << "      \"name\": \"" << escapeJSON(func.name) << "\",\n";
        out << "      \"nodes\": [\n";
        
        bool first_node = true;
        std::map<std::string, std::vector<int>> def_chains; // Tracks which Stmt IDs defined which var

        for (const auto& block : func.blocks) {
            for (const auto& stmt : block.statements) {
                if (!first_node) out << ",\n";
                first_node = false;

                // Feature Extraction
                int opcode = getOpcodeEmbedding(stmt.type);
                bool side_effect = hasSideEffect(stmt);
                
                std::string lhs = extractLHS(stmt.text);
                if (!lhs.empty()) {
                    def_chains[lhs].push_back(stmt.id);
                }
                
                out << "        {\n";
                out << "          \"id\": " << stmt.id << ",\n";
                out << "          \"block_id\": " << block.id << ",\n";
                out << "          \"text\": \"" << escapeJSON(stmt.text) << "\",\n";
                out << "          \"features\": [" << opcode << ", " << (side_effect ? 1 : 0) << "]\n";
                out << "        }";
            }
        }
        out << "\n      ],\n";
        
        // Build Dataflow & Control Flow Edges
        out << "      \"edges\": [\n";
        bool first_edge = true;

        for (const auto& block : func.blocks) {
            int last_stmt_id = -1;
            for (const auto& stmt : block.statements) {
                // 1. Control Flow Edge (Sequential within Basic Block)
                if (last_stmt_id != -1) {
                    if (!first_edge) out << ",\n";
                    out << "        {\"source\": " << last_stmt_id << ", \"target\": " << stmt.id << ", \"type\": \"CFG\"}";
                    first_edge = false;
                }
                last_stmt_id = stmt.id;

                // 2. Data Flow Edge (Def-Use Chains)
                auto rhs_vars = extractRHS(stmt.text);
                for (const auto& var : rhs_vars) {
                    if (def_chains.count(var)) {
                        for (int def_id : def_chains[var]) {
                            if (!first_edge) out << ",\n";
                            out << "        {\"source\": " << def_id << ", \"target\": " << stmt.id << ", \"type\": \"DFG\"}";
                            first_edge = false;
                        }
                    }
                }
            }
            
            // 3. Control Flow Edge (Across Basic Blocks)
            if (last_stmt_id != -1) {
                for (int succ_block_id : block.successors) {
                    // Find first statement of successor block
                    int target_stmt_id = -1;
                    for (const auto& succ_block : func.blocks) {
                        if (succ_block.id == succ_block_id && !succ_block.statements.empty()) {
                            target_stmt_id = succ_block.statements[0].id;
                            break;
                        }
                    }
                    if (target_stmt_id != -1) {
                        if (!first_edge) out << ",\n";
                        out << "        {\"source\": " << last_stmt_id << ", \"target\": " << target_stmt_id << ", \"type\": \"CFG_BRANCH\"}";
                        first_edge = false;
                    }
                }
            }
        }
        
        out << "\n      ]\n    }";
        if (f_idx < prog.functions.size() - 1) out << ",";
        out << "\n";
    }

    out << "  ]\n}\n";
    out.close();
    std::cout << "[ML Pipeline] Successfully exported IR Graph features to " << out_path << "\n";
}
