#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem> // Requires C++17
#include "ir.h"

namespace fs = std::filesystem;

extern int yyparse();

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
    // Set folder name: use first argument if provided, else default
    std::string projectName = (argc > 1) ? argv[1] : "analysis_report";
    
    if (yyparse() == 0) {
        // 1. Create Directory
        if (!fs::exists(projectName)) {
            fs::create_directory(projectName);
        }

        // 2. Generate Summary Report
        std::ofstream report(projectName + "/summary.txt");
        report << "GIMPLE SSA ANALYSIS REPORT\n" << std::string(30, '=') << "\n";
        report << "Number of functions: " << program.functions.size() << "\n\n";

        for (const auto& func : program.functions) {
            report << "Function: " << func.name << "\n";
            report << "  Preamble: " << func.preamble.size() << " stmts\n";
            for (const auto& block : func.blocks) {
                report << "    Block " << block.id << " (" << block.statements.size() << " stmts)\n";
                report << "      Successors: ";
                for (int s : block.successors) report << s << " ";
                report << "\n";
            }
            report << "\n";
        }
        report.close();

        // 3. Generate DOT file
        export_to_dot(program, projectName + "/graph.dot");

        std::cout << "Analysis complete. Files generated in folder: ./" << projectName << std::endl;
        std::cout << "To generate image, run: dot -Tpng " << projectName << "/graph.dot -o " << projectName << "/graph.png" << std::endl;

    } else {
        std::cerr << "Parsing failed." << std::endl;
        return 1;
    }
    return 0;
}

//main file that compiles the program