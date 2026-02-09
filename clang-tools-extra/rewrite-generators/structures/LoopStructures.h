//
// Data structures for loop rewriting
//

#ifndef LLVM_REWRITE_GENERATORS_LOOPSTRUCTURES_H
#define LLVM_REWRITE_GENERATORS_LOOPSTRUCTURES_H

#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include <string>

using namespace clang;

struct RangedForLoop {
    const CXXForRangeStmt *stmt;
    std::string loopVarName;
    std::string loopVarType;
    Expr *rangeExpr;
    std::string rangeVarName;  // e.g., "__range_0"
    std::string beginVarName;  // e.g., "__begin_0"
    std::string endVarName;    // e.g., "__end_0"
    unsigned index;
    SourceRange fullRange;
};

#endif // LLVM_REWRITE_GENERATORS_LOOPSTRUCTURES_H
