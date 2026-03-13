//
// Generic AST utility functions (reusable across any Clang tool)
//

#ifndef LLVM_REWRITE_GENERATORS_ASTHELPERS_H
#define LLVM_REWRITE_GENERATORS_ASTHELPERS_H

#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include <string>

using namespace clang;

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
inline const Expr* unwrapExpr(const Expr* E, bool IgnoreImplicitCasts = true) {
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

/// Gets the source text for an expression
inline std::string getSourceText(const Expr *expr, const SourceManager &sourceManager) {
    SourceRange rangeRange = expr->getSourceRange();
    CharSourceRange charRange = CharSourceRange::getTokenRange(rangeRange);
    return Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
}

/// Checks if an expression is a prvalue (after unwrapping)
inline bool isPrValue(const Expr* initializer) {
    if (!initializer) {
        return false;
    }
    initializer = unwrapExpr(initializer);
    if (initializer->getValueKind() == VK_PRValue) {
        return true;
    }
    // An InitListExpr is a prvalue only if it has multiple elements (aggregate init)
    // or if it has a single element that is itself a prvalue.
    // Single-element InitListExpr with an lvalue element is reference binding (e.g., auto& x{b})
    if (const auto *initList = dyn_cast<InitListExpr>(initializer)) {
        if (initList->getNumInits() == 1) {
            return isPrValue(initList->getInit(0));
        }
        // Zero or multiple elements: aggregate/value init, treat as prvalue
        return true;
    }
    return false;
}

/// Converts a type to a fully qualified string
inline std::string typeAsString(const QualType& type, const ASTContext &astContext) {
    QualType canonicalType = type.getCanonicalType();

    PrintingPolicy policy(astContext.getLangOpts());
    policy.SuppressScope = false;
    policy.SuppressTagKeyword = false;
    policy.SuppressUnwrittenScope = false;
    policy.FullyQualifiedName = true;
    policy.PrintCanonicalTypes = true;

    return canonicalType.getAsString(policy);
}

/// Gets the appropriate type for an auto&& variable based on the initializer
inline std::pair<std::string, bool> getTypeForAutoRefRefVariable(const Expr* initializer, const ASTContext &astContext) {
    initializer = unwrapExpr(initializer);
    const auto& tp = initializer->getType();
    if (tp->isReferenceType()) {
        return {typeAsString(tp, astContext), false};
    }
    auto prefix = isPrValue(initializer) ? "std::add_rvalue_reference_t<" : "std::add_lvalue_reference_t<";
    return {prefix + typeAsString(tp, astContext) + ">", isPrValue(initializer)};
}

/// Extracts the original argument from a coroutine suspend expression.
/// In a non-dependent context, the operand is `promise.yield_value(x)` — a CallExpr
/// whose first argument is the user-written expression.
/// In a dependent (template) context, the operand IS the user-written expression directly
/// (e.g., just `a`), because the promise call can't be resolved yet.
inline const Expr *getOriginalCoroutineExprArgument(const CoroutineSuspendExpr *expr) {
    if (!expr) return nullptr;

    const Expr *operand = expr->getOperand();
    if (!operand) return nullptr;

    // In dependent context, the operand is the user-written expression directly
    if (expr->getType()->isDependentType()) {
        return operand->IgnoreImplicit();
    }

    operand = operand->IgnoreImplicit();

    // Non-dependent: extract argument from promise.yield_value(x) / await_transform(x)
    if (const auto *call = dyn_cast<CallExpr>(operand)) {
        if (call->getNumArgs() > 0) {
            return call->getArg(0)->IgnoreImplicit();
        }
    }
    llvm::errs() << "WARNING: call to yield_value or similar is not a CallExpr, "
                 << "operand class: " << operand->getStmtClassName() << "\n";
    return operand;
}

#endif // LLVM_REWRITE_GENERATORS_ASTHELPERS_H
