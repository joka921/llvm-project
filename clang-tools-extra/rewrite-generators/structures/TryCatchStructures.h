//
// Data structures for try-catch block rewriting
//

#ifndef LLVM_REWRITE_GENERATORS_TRYCATCHSTRUCTURES_H
#define LLVM_REWRITE_GENERATORS_TRYCATCHSTRUCTURES_H

#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include <string>
#include <vector>

using namespace clang;

struct TryCatchBlock {
    const CXXTryStmt *tryStmt;
    unsigned index;
    SourceLocation tryKeywordLoc;
    SourceLocation tryBlockStart;  // Opening brace of try block
    SourceLocation tryBlockEnd;    // Closing brace of try block
    std::vector<std::string> catchClauses;  // Transformed catch clause bodies
    SourceLocation catchEnd;       // End of all catch clauses
    std::vector<std::string> variablesInTryBlock;  // Variables declared inside this try block, in reverse order
};

#endif // LLVM_REWRITE_GENERATORS_TRYCATCHSTRUCTURES_H
