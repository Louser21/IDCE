#ifndef IR_H
#define IR_H

#include <string>
#include <vector>

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

#endif