//
// Data structures for loop rewriting
//

#ifndef LLVM_REWRITE_GENERATORS_LOOPSTRUCTURES_H
#define LLVM_REWRITE_GENERATORS_LOOPSTRUCTURES_H

#include "clang/AST/Stmt.h"
#include "clang/AST/StmtCXX.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <tuple>

using namespace clang;

// Forward declarations
struct ScopeEndReplacement;

// Prioritized replacement type (SourceRange, replacement_string, priority, isReplace)
using PrioritizedReplacement = std::tuple<SourceRange, std::string, int, bool>;

// Return type for ranged-for loop replacements
struct RangedForReplacements {
    std::vector<PrioritizedReplacement> globalReplacements;
    std::map<unsigned, std::vector<ScopeEndReplacement>> scopeEndReplacements;
};

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

    // Main method to collect all replacements for this ranged-for loop
    RangedForReplacements collectReplacements(
        const SourceManager &SM,
        std::function<std::string(const Expr*)> rewriteExprFunc,
        std::function<SourceLocation(SourceLocation)> getLocForEndOfTokenFunc
    ) const;

private:
    // Helper methods for collecting different parts of the replacements
    void collectHeaderReplacement(
        const SourceManager &SM,
        std::function<std::string(const Expr*)> rewriteExprFunc,
        RangedForReplacements &result
    ) const;

    void collectLoopVarConstruct(
        const SourceManager &SM,
        std::function<SourceLocation(SourceLocation)> getLocForEndOfTokenFunc,
        RangedForReplacements &result
    ) const;

    void collectLoopVarDestroy(
        const SourceManager &SM,
        RangedForReplacements &result
    ) const;

    void collectFooterInsertions(
        const SourceManager &SM,
        RangedForReplacements &result
    ) const;
};

#endif // LLVM_REWRITE_GENERATORS_LOOPSTRUCTURES_H
