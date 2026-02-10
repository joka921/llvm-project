//
// Data structures for try-catch block rewriting
//

#ifndef LLVM_REWRITE_GENERATORS_TRYCATCHSTRUCTURES_H
#define LLVM_REWRITE_GENERATORS_TRYCATCHSTRUCTURES_H

#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include <functional>
#include <string>
#include <tuple>
#include <vector>

using namespace clang;

struct TryCatchBlock {
    const CXXTryStmt *tryStmt;
    unsigned index;
    unsigned resumeIndex;  // goto label index, from same counter as suspension points
    SourceLocation tryKeywordLoc;
    SourceLocation tryBlockStart;  // Opening brace of try block
    SourceLocation tryBlockEnd;    // Closing brace of try block
    std::vector<std::string> catchClauses;  // Transformed catch clause bodies
    SourceLocation catchEnd;       // End of all catch clauses
    std::vector<std::string> variablesInTryBlock;  // Variables declared inside this try block, in reverse order

    // Generate replacements for this try-catch block
    std::vector<std::tuple<SourceRange, std::string, int, bool>> generateReplacements(
        const SourceManager &sourceManager,
        std::function<SourceLocation(SourceLocation)> getLocForEndOfToken
    ) const;
};

#endif // LLVM_REWRITE_GENERATORS_TRYCATCHSTRUCTURES_H
