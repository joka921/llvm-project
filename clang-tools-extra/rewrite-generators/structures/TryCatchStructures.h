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
    unsigned parentIndex = static_cast<unsigned>(-1);  // index of enclosing try block, or UINT_MAX if none
    SourceLocation tryKeywordLoc;
    SourceLocation tryBlockStart;  // Opening brace of try block
    SourceLocation tryBlockEnd;    // Closing brace of try block
    std::vector<std::string> variablesInTryBlock;  // Variables declared inside this try block, in reverse order

    // Suspension points directly contained in this try block (not in nested try blocks)
    std::vector<unsigned> directSuspensionIndices;
    // Indices of directly-nested child try blocks
    std::vector<unsigned> nestedTryIndices;

    // Generate replacements for this try-catch block
    // Needs the full tryCatchBlocks vector for computing transitive states in nested try blocks
    std::vector<std::tuple<SourceRange, std::string, int, bool>> generateReplacements(
        const SourceManager &sourceManager,
        std::function<SourceLocation(SourceLocation)> getLocForEndOfToken,
        const std::vector<TryCatchBlock> &allBlocks
    ) const;

private:
    // Collect all suspension indices transitively contained in a try block (including nested children)
    static void collectTransitiveStates(unsigned blockIndex,
                                        const std::vector<TryCatchBlock> &allBlocks,
                                        std::vector<unsigned> &out);
};

#endif // LLVM_REWRITE_GENERATORS_TRYCATCHSTRUCTURES_H
