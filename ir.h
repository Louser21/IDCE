#ifndef IR_H
#define IR_H

#include <string>
#include <vector>
#include <set>

enum StmtType {
    STMT_ASSIGN,
    STMT_CALL,
    STMT_GOTO,
    STMT_RETURN,
    STMT_COND,
    STMT_OTHER
};

struct Statement {
    int id;
    std::string text;
    StmtType type;
};

struct BasicBlock {
    int id;
    std::vector<Statement> statements;
    std::vector<int> successors;
};

struct FunctionIR {
    std::string name;
    std::vector<Statement> preamble;
    std::vector<BasicBlock> blocks;
};

struct ProgramIR {
    std::vector<FunctionIR> functions;
};

extern ProgramIR program;

// --- Optimization Pass Declarations ---

/**
 * applyGlobalDCE: The main entry point for Week 7 Core Logic.
 * Performs: removeUnusedFunctions -> applyConstantFolding -> removeUnreachableBlocks -> applyLocalDCE.
 */
void applyGlobalDCE(ProgramIR& prog);

bool hasSideEffect(const Statement& stmt);

/**
 * applyDCE: Performs variable liveness analysis (Mark-and-Sweep) on a program.
 */
void applyDCE(ProgramIR& prog);

/**
 * applyLocalDCE: Optimized per-function pass that integrates logic folding and liveness.
 */
void applyLocalDCE(FunctionIR& func);

// --- Helper Declarations ---
std::string extractLHS(const std::string& text);
std::set<std::string> extractRHS(const std::string& text);
std::string sanitizeVar(std::string var);

/**
 * applyIntelligentDCE: ML Guided pass
 */
void applyIntelligentDCE(ProgramIR& prog, const std::set<int>& dead_ids);

#endif