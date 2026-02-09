//
// LambdaRewriter - Converts lambda expressions into equivalent functor classes
//

#ifndef LLVM_REWRITE_GENERATORS_LAMBDAREWRITER_H
#define LLVM_REWRITE_GENERATORS_LAMBDAREWRITER_H

#include "clang/AST/ExprCXX.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/AST/ASTContext.h"

#include <string>
#include <vector>

using namespace clang;

/// Result of rewriting a lambda expression into a functor class
struct LambdaRewriteResult {
    std::string classDefinition;   // Full struct definition text
    std::string constructorCall;   // Aggregate-init expression, e.g. __lambda_L42_C5{x, y}
    std::string className;         // The generated name, e.g. __lambda_L42_C5
};

/// Internal representation of a single lambda capture
struct CaptureInfo {
    std::string memberName;        // Name used as struct member
    std::string memberType;        // Type string for the member declaration
    std::string initExpression;    // Source text for the constructor call argument
    bool isReference;              // Whether the member is a reference
    bool isThisCapture;            // Captures `this` pointer
    bool isStarThisCapture;        // Captures `*this` (by copy)
};

/// Rewrites a LambdaExpr into an equivalent functor struct definition
/// and an aggregate-initialization constructor call.
class LambdaRewriter {
public:
    LambdaRewriter(const SourceManager &SM, ASTContext &ctx);
    LambdaRewriteResult rewriteLambda(const LambdaExpr *lambda);

private:
    const SourceManager &sourceManager;
    ASTContext &astContext;

    std::string generateClassName(const LambdaExpr *lambda);
    std::vector<CaptureInfo> analyzeCaptures(const LambdaExpr *lambda);
    std::string getOperatorSignature(const LambdaExpr *lambda);
    std::string getBodyText(const LambdaExpr *lambda);
    std::string generateClassDefinition(const LambdaExpr *lambda,
                                         const std::string &className,
                                         const std::vector<CaptureInfo> &captures);
    std::string generateConstructorCall(const std::string &className,
                                         const std::vector<CaptureInfo> &captures);
};

#endif // LLVM_REWRITE_GENERATORS_LAMBDAREWRITER_H
