//
// Data structures for scope tracking and management
//

#ifndef LLVM_REWRITE_GENERATORS_SCOPESTRUCTURES_H
#define LLVM_REWRITE_GENERATORS_SCOPESTRUCTURES_H

#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include <string>
#include <vector>

using namespace clang;

struct ScopeInfo {
    const CompoundStmt *compoundStmt;
    std::vector<std::string> variablesInScope;
    SourceLocation scopeEnd;

    // Loop tracking information
    bool isLoopScope = false;
    bool isLoopBodyScope = false;  // true if this is the body of a loop (not the header/init)
    const Stmt *loopStmt = nullptr;  // The loop statement this scope belongs to
};

struct ScopeEndReplacement {
    std::string replacement;
    int priority;
    bool insertAfterBrace = false;

    bool operator<(const ScopeEndReplacement &other) const {
        return insertAfterBrace != other.insertAfterBrace ? other.insertAfterBrace : priority < other.priority;
    }
};

#endif // LLVM_REWRITE_GENERATORS_SCOPESTRUCTURES_H
