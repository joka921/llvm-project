//
// Data structures for coroutine analysis and rewriting
//

#ifndef LLVM_REWRITE_GENERATORS_COROUTINESTRUCTURES_H
#define LLVM_REWRITE_GENERATORS_COROUTINESTRUCTURES_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceLocation.h"
#include "LoopStructures.h"
#include "TryCatchStructures.h"
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace clang;

struct FunctionParameter {
    std::string name;
    std::string type;
    QualType qualType;
};

struct TemporaryInfo {
    const MaterializeTemporaryExpr *expr; // The MaterializeTemporaryExpr node
    std::string tempVarName;             // e.g., temp_1_0
    QualType type;                       // Type of the temporary
    SourceLocation constructLoc;         // Where to insert the CO_INIT macro
    std::string initArgs;                // Arguments for the initialization
    bool isBracedInit;                   // true for CO_BRACED_INIT_OWNING, false for CO_PAREN_INIT_OWNING
};

struct LambdaInCoroutine {
    std::string classDefinition;          // Functor struct text
    std::string constructorCall;          // Constructor call with CO_GET captures
    std::string className;                // e.g. __Lambda_L3_C16 (struct type name)
    std::string memberName;               // e.g. __lambda_L3_C16 (member variable name)
    SourceRange lambdaSourceRange;        // Source range of the lambda expression
    std::string originalVarName;          // Name of the variable this lambda initializes (empty if none)
    SourceLocation varDeclLocation;       // Location of that variable's declaration
};

struct CoroutineStatement {
    enum Type { YIELD, AWAIT, RETURN };

    Type type;
    const Stmt *stmt; // CoawaitExpr*, CoyieldExpr*, or CoreturnStmt*
    const Expr *operand; // The expression being yielded/awaited/returned
    unsigned index;
    std::string awaiterMemberName; // e.g., "__awaiter_1"
    SourceLocation keywordLoc;
    SourceLocation operandStart;
    SourceLocation operandEnd;
    std::vector<std::string> aliveVariables; // Variables alive at this suspension point, in reverse order of declaration
    std::vector<TemporaryInfo> temporaries; // Subexpression temporaries that need lifetime extension

    // Generate replacements for this coroutine statement
    std::vector<std::tuple<SourceRange, std::string, int, bool>> generateReplacements(
        const SourceManager &sourceManager,
        ASTContext &astContext
    ) const;

private:
    std::vector<std::tuple<SourceRange, std::string, int, bool>> generateYieldOrAwaitReplacements(
        const SourceManager &sourceManager,
        ASTContext &astContext,
        const std::string &macroBaseName
    ) const;

    std::vector<std::tuple<SourceRange, std::string, int, bool>> generateReturnReplacements(
        const SourceManager &sourceManager,
        ASTContext &astContext
    ) const;
};

struct LocalVariable {
    std::string name;
    std::string type; // Original type (may be reference)
    bool isOwning; // Type to store in _coro_storage (first template parameter)
    std::string referenceType; // Reference type for access (second template parameter)
    bool isReference; // True if original type is a reference
    SourceLocation location;
    int priority; // Position in file for ordering by first appearance

    bool operator<(const LocalVariable &other) const {
        // First compare by priority (file position), then by name for deterministic ordering
        if (priority != other.priority) {
            return priority < other.priority;
        }
        return name < other.name;
    }
};

struct CoroutineInfo {
    const FunctionDecl *function;
    std::set<LocalVariable> localVariables;
    std::vector<RangedForLoop> rangedForLoops;
    std::vector<TryCatchBlock> tryCatchBlocks;
    std::vector<CoroutineStatement> coroutineStatements;
    std::vector<FunctionParameter> parameters;
    SourceLocation insertionPoint;
    bool hasError = false;

    // Member function information
    bool isMemberFunction = false;
    bool isConstMemberFunction = false;
    std::string className;

    // Lambda information
    bool isLambda = false;
    const LambdaExpr *lambdaExpr = nullptr;

    // Mapping from variable declaration location to member name (for handling shadowing)
    std::map<SourceLocation, std::string> declLocationToMemberName;

    // Regular lambdas found inside the coroutine body
    std::vector<LambdaInCoroutine> lambdasInBody;

    // Mapping from variable declaration location to lambda info (for lambda variable renaming)
    std::map<SourceLocation, LambdaInCoroutine> lambdaVariableMapping;

    // Variables alive at the outermost function body scope end (for falloff case in destroySuspendedCoro)
    std::vector<std::string> outermostScopeVariables;
};

#endif // LLVM_REWRITE_GENERATORS_COROUTINESTRUCTURES_H
