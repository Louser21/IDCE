#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include "ir.h"

namespace fs = std::filesystem;

extern int yyparse();
void applyGlobalDCE(ProgramIR& prog);
void exportIRFeaturesToJSON(const ProgramIR& prog, const std::string& out_path);

void export_to_dot(const ProgramIR& prog, const std::string& filepath) {
    std::ofstream out(filepath);
    out << "digraph G {\n";
    out << "  node [shape=box, fontname=\"Courier\", style=filled, fillcolor=white];\n";
    out << "  compound=true;\n";

    int func_counter = 0;
    for (const auto& func : prog.functions) {
        func_counter++;
        std::string cluster_id = "cluster_" + std::to_string(func_counter);

        out << "  subgraph \"" << cluster_id << "\" {\n";
        out << "    label = \"" << func.name << "\";\n";
        out << "    style = dashed; color = blue;\n";

        for (const auto& block : func.blocks) {
            std::string node_id = "node_" + std::to_string(func_counter) + "_" + std::to_string(block.id);
            out << "    " << node_id << " [label=\"BB " << block.id << "\\n";
            for (const auto& stmt : block.statements) {
                std::string clean = stmt.text;
                size_t p = 0;
                while ((p = clean.find('"', p)) != std::string::npos) { clean.replace(p, 1, "\\\""); p += 2; }
                out << clean << "\\n";
            }
            out << "\"];\n";

            for (int succ : block.successors) {
                std::string target_id = "node_" + std::to_string(func_counter) + "_" + std::to_string(succ);
                out << "    " << node_id << " -> " << target_id << ";\n";
            }
        }
        out << "  }\n";
    }
    out << "}\n";
}

int main(int argc, char** argv) {
    bool ml_extract = false;
    bool ml_dce = false;
    std::string projectName = "analysis_report";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ml-extract") ml_extract = true;
        else if (arg == "--ml-dce") ml_dce = true;
        else if (arg.find("--") == std::string::npos) projectName = arg;
    }

    if (yyparse() == 0) {
        if (ml_extract) {
            if (!fs::exists(projectName)) fs::create_directory(projectName);
            exportIRFeaturesToJSON(program, projectName + "/ir_graph_features.json");
        }

        if (ml_dce && !ml_extract) {
            std::cout << "[ML Pipeline] Starting ML Inference DCE Pass...\n";
            if (!fs::exists(projectName)) fs::create_directory(projectName);
            std::string tmp_graph = projectName + "/tmp_ir_graph.json";
            std::string tmp_out   = projectName + "/tmp_ml_preds.json";

            exportIRFeaturesToJSON(program, tmp_graph);
            std::string cmd = ". ml/.venv/bin/activate && python3 ml/inference.py " + tmp_graph + " ml_data/dce_model.pt > " + tmp_out;
            int ret = std::system(cmd.c_str());

            if (ret != 0) {
                std::cerr << "[ML Error] Inference script failed.\n";
            } else {
                std::ifstream ifs(tmp_out);
                std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
                std::set<int> dead_ids;
                size_t p = content.find('[');
                if (p != std::string::npos) {
                    p++;
                    while (p < content.size() && content[p] != ']') {
                        if (isdigit(content[p])) {
                            size_t end;
                            int id = std::stoi(content.substr(p), &end);
                            dead_ids.insert(id);
                            p += end;
                        } else {
                            p++;
                        }
                    }
                }
                applyIntelligentDCE(program, dead_ids);
                std::cout << "[ML Pipeline] Inference complete. Evaluated " << dead_ids.size() << " predicted dead instructions.\n";
            }
        } else {
            applyGlobalDCE(program);
        }

        if (ml_extract) {
            exportIRFeaturesToJSON(program, projectName + "/ir_graph_optimized.json");
            return 0;
        }

        if (!fs::exists(projectName)) {
            fs::create_directory(projectName);
        }

        std::ofstream report(projectName + "/summary.txt");
        report << "GIMPLE SSA ANALYSIS REPORT (OPTIMIZED)\n" << std::string(35, '=') << "\n";
        report << "Final number of reachable functions: " << program.functions.size() << "\n\n";

        for (const auto& func : program.functions) {
            report << "Function: " << func.name << "\n";
            report << "  Preamble: " << func.preamble.size() << " declarations remaining\n";
            for (const auto& block : func.blocks) {
                report << "    Block " << block.id << " (" << block.statements.size() << " live stmts)\n";
                report << "      Successors: ";
                for (int s : block.successors) report << s << " ";
                report << "\n";
            }
            report << "\n";
        }
        report.close();

        export_to_dot(program, projectName + "/graph.dot");

        std::cout << "Analysis complete. Intermediate logs generated." << std::endl;
        std::cout << "Files generated in folder: ./" << projectName << std::endl;
        std::cout << "To generate image, run: dot -Tpng " << projectName << "/graph.dot -o " << projectName << "/graph.png" << std::endl;

    } else {
        std::cerr << "Parsing failed." << std::endl;
        return 1;
    }
    return 0;
}