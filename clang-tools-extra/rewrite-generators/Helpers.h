//
// Created by kalmbacj on 2025-09-15.
//

#ifndef LLVM_HELPERS_H
#define LLVM_HELPERS_H
using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

/// Strips off implicit AST nodes to get to the original subexpression.
///
/// This removes wrappers like:
/// - MaterializeTemporaryExpr
/// - ExprWithCleanups
/// - CXXBindTemporaryExpr
/// - FullExpr
/// - ConstantExpr
/// - ImplicitCastExpr (optionally)
///
/// @param E The expression to unwrap.
/// @param IgnoreImplicitCasts Whether to remove implicit casts (true by default).
/// @return The unwrapped subexpression.
const Expr* unwrapExpr(const Expr* E, bool IgnoreImplicitCasts = true) {
    while (true) {
        if (auto* MTE = dyn_cast<MaterializeTemporaryExpr>(E)) {
            E = MTE->getSubExpr();
        } else if (auto* EWCE = dyn_cast<ExprWithCleanups>(E)) {
            E = EWCE->getSubExpr();
        } else if (auto* BTE = dyn_cast<CXXBindTemporaryExpr>(E)) {
            E = BTE->getSubExpr();
        } else if (auto* FE = dyn_cast<FullExpr>(E)) {
            E = FE->getSubExpr();
        } else if (auto* CE = dyn_cast<ConstantExpr>(E)) {
            E = CE->getSubExpr();
        } else if (IgnoreImplicitCasts) {
            if (auto* ICE = dyn_cast<ImplicitCastExpr>(E)) {
                E = ICE->getSubExpr();
            } else {
                break;
            }
        } else {
            break;
        }
    }
    return E;
}

std::string getSourceText(const Expr *expr, const SourceManager &sourceManager) {
    SourceRange rangeRange = expr->getSourceRange();
    CharSourceRange charRange = CharSourceRange::getTokenRange(rangeRange);
    return Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
}

 bool isPrValue(const Expr* initializer) {
    if (!initializer) {
        return false;
    }
    //initializer->dump();
    initializer = unwrapExpr(initializer);
    return initializer->getValueKind() == VK_PRValue || (dyn_cast<InitListExpr>(initializer) != nullptr);
}
std::string typeAsString(const QualType& type, const ASTContext &astContext) {
    // Get the canonical type (resolves typedefs, auto, etc.)
    QualType canonicalType = type.getCanonicalType();

    // Use a printing policy that produces fully qualified names
    PrintingPolicy policy(astContext.getLangOpts());
    policy.SuppressScope = false; // Include scope information
    policy.SuppressTagKeyword = false; // Keep 'struct', 'class', etc.
    policy.SuppressUnwrittenScope = false; // Include all scopes
    policy.FullyQualifiedName = true; // Force fully qualified names
    policy.PrintCanonicalTypes = true; // Print canonical types

    return canonicalType.getAsString(policy);
}

std::pair<std::string, bool> getTypeForAutoRefRefVariable(const Expr* initializer, const ASTContext &astContext) {
    initializer = unwrapExpr(initializer);
    const auto& tp = initializer->getType();
    if (tp->isReferenceType()) {
        return {typeAsString(tp, astContext), false};
    }
    auto prefix = isPrValue(initializer) ? "std::add_rvalue_reference_t<" : "std::add_lvalue_reference_t<";
    return {prefix + typeAsString(tp, astContext) + ">", isPrValue(initializer)};
}

#endif //LLVM_HELPERS_H