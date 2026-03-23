//
// Decltype expression generator for dependent types in template coroutines
//

#ifndef LLVM_REWRITE_GENERATORS_DECLTYPEGENERATOR_H
#define LLVM_REWRITE_GENERATORS_DECLTYPEGENERATOR_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "../structures/CoroutineStructures.h"
#include "../Common.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace clang;

struct DecltypeResult {
    std::string refTypeExpr;   // Type expression for _coro_storage first template param
    std::string isOwningExpr;  // Expression for _coro_storage second template param
};

class DecltypeExpressionGenerator {
private:
    const SourceManager &sourceManager;
    ASTContext &astContext;
    const std::vector<FunctionParameter> &parameters;
    bool isMemberFunction;

    struct RefSubstitution {
        unsigned offset; // Offset from start of init expression source text
        unsigned length; // Length of original text to replace
        std::string replacement; // Replacement text
    };

    std::string substituteParameter(const std::string &name) const {
        return "std::declval<decltype(" + name + ")&>()";
    }

    std::string substituteLocalVariable(const std::string &name) const {
        return "std::declval<decltype(std::declval<decltype(" + name + ")&>().get().ref_)>()";
    }

    std::string substituteThis() const {
        return "std::declval<decltype(__self)>()";
    }

    bool isParameter(const std::string &name) const {
        for (const auto &param : parameters) {
            if (param.name == name) return true;
        }
        return false;
    }

    /// Get the source text offset of an expression relative to the init expression start
    unsigned getRelativeOffset(SourceLocation loc, SourceLocation initStart) const {
        unsigned initOffset = sourceManager.getFileOffset(initStart);
        unsigned exprOffset = sourceManager.getFileOffset(loc);
        return exprOffset - initOffset;
    }

    /// Get the source text length of a token at a given location
    unsigned getTokenLength(SourceLocation loc) const {
        return Lexer::MeasureTokenLength(loc, sourceManager, astContext.getLangOpts());
    }

    /// Recursively find all DeclRefExpr and CXXThisExpr nodes in an expression
    void findReferences(const Stmt *stmt, SourceLocation initStart,
                        const std::set<std::string> &declaredVarNames,
                        std::vector<RefSubstitution> &subs) const {
        if (!stmt) return;

        // Don't recurse into lambda bodies
        if (isa<LambdaExpr>(stmt)) return;

        if (const auto *declRef = dyn_cast<DeclRefExpr>(stmt)) {
            const ValueDecl *decl = declRef->getDecl();
            std::string name = decl->getNameAsString();
            SourceLocation loc = declRef->getLocation();

            // Only process if in the same file (not macro-expanded to somewhere else)
            if (!sourceManager.isWrittenInSameFile(loc, initStart)) return;

            unsigned offset = getRelativeOffset(loc, initStart);
            unsigned length = getTokenLength(loc);

            if (isa<ParmVarDecl>(decl)) {
                subs.push_back({offset, length, substituteParameter(name)});
            } else if (isa<VarDecl>(decl) && declaredVarNames.count(name)) {
                subs.push_back({offset, length, substituteLocalVariable(name)});
            }
            return; // DeclRefExpr has no interesting children
        }

        if (const auto *thisExpr = dyn_cast<CXXThisExpr>(stmt)) {
            if (isMemberFunction) {
                SourceLocation loc = thisExpr->getLocation();
                if (sourceManager.isWrittenInSameFile(loc, initStart)) {
                    unsigned offset = getRelativeOffset(loc, initStart);
                    // 'this' is 4 characters
                    subs.push_back({offset, 4, substituteThis()});
                }
            }
            return;
        }

        // Recurse into children
        for (const Stmt *child : stmt->children()) {
            if (child) {
                findReferences(child, initStart, declaredVarNames, subs);
            }
        }
    }

    /// Apply substitutions to source text in reverse offset order
    std::string applySubstitutions(const std::string &sourceText,
                                   std::vector<RefSubstitution> &subs) const {
        // Sort by offset descending so replacements don't shift later offsets
        std::sort(subs.begin(), subs.end(),
                  [](const RefSubstitution &a, const RefSubstitution &b) {
                      return a.offset > b.offset;
                  });

        std::string result = sourceText;
        for (const auto &sub : subs) {
            if (sub.offset + sub.length <= result.size()) {
                result.replace(sub.offset, sub.length, sub.replacement);
            }
        }
        return result;
    }

public:
    DecltypeExpressionGenerator(const SourceManager &SM, ASTContext &ctx,
                                const std::vector<FunctionParameter> &params,
                                bool isMember)
        : sourceManager(SM), astContext(ctx), parameters(params),
          isMemberFunction(isMember) {}

    DecltypeResult generateForAutoVar(const VarDecl *varDecl,
                                      LocalVariable::AutoQualifier qualifier,
                                      const std::set<std::string> &declaredVarNames) const {
        const Expr *init = varDecl->getInit();
        if (!init) {
            REWRITE_LOG() << "    WARNING: dependent auto var '" << varDecl->getNameAsString()
                         << "' has no initializer\n";
            return {"/* ERROR: no initializer */", "true"};
        }

        // Get source text of the initializer
        SourceRange initRange = init->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(initRange);
        std::string sourceText = Lexer::getSourceText(charRange, sourceManager,
                                                       astContext.getLangOpts()).str();

        REWRITE_LOG() << "    Generating decltype for '" << varDecl->getNameAsString()
                     << "' with init: " << sourceText << "\n";

        // Find all references that need substitution
        std::vector<RefSubstitution> subs;
        SourceLocation initStart = initRange.getBegin();
        findReferences(init, initStart, declaredVarNames, subs);

        // Apply substitutions
        std::string modifiedText = applySubstitutions(sourceText, subs);

        REWRITE_LOG() << "    Modified init text: " << modifiedText << "\n";

        // Build refTypeExpr
        std::string refTypeExpr = "std::add_lvalue_reference_t<std::remove_reference_t<decltype("
                                  + modifiedText + ")>>";

        // Build isOwningExpr based on qualifier
        std::string isOwningExpr;
        switch (qualifier) {
            case LocalVariable::AutoQualifier::Plain:
                isOwningExpr = "true";
                break;
            case LocalVariable::AutoQualifier::LRef:
                isOwningExpr = "false";
                break;
            case LocalVariable::AutoQualifier::RRef:
                isOwningExpr = "!std::is_lvalue_reference_v<decltype((" + modifiedText + "))>";
                break;
            case LocalVariable::AutoQualifier::ConstLRef:
                isOwningExpr = "!std::is_reference_v<decltype((" + modifiedText + "))>";
                break;
            case LocalVariable::AutoQualifier::None:
                isOwningExpr = "true"; // Shouldn't happen, but default to owning
                break;
        }

        REWRITE_LOG() << "    refTypeExpr: " << refTypeExpr << "\n";
        REWRITE_LOG() << "    isOwningExpr: " << isOwningExpr << "\n";

        return {refTypeExpr, isOwningExpr};
    }
};

#endif // LLVM_REWRITE_GENERATORS_DECLTYPEGENERATOR_H
