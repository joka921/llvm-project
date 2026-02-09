//
// CoroutineRewriter - Main orchestrator for coroutine rewriting
//

#ifndef LLVM_REWRITE_GENERATORS_COROUTINEREWRITER_H
#define LLVM_REWRITE_GENERATORS_COROUTINEREWRITER_H

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LangOptions.h"

#include "Common.h"
#include "structures/CoroutineStructures.h"
#include <vector>

using namespace clang;

class CoroutineBodyRewriter;

class CoroutineRewriter : public RecursiveASTVisitor<CoroutineRewriter> {
private:
    const SourceManager &sourceManager;
    Rewriter &rewriter;
    clang::DiagnosticsEngine &diagnosticsEngine;
    const LangOptions &langOptions;
    ASTContext *astContext;
    std::vector<CoroutineInfo> coroutines;

    // Private helper methods
    bool containsCoroutineKeywords(const Stmt *stmt);
    bool containsTryCatchBlocks(const Stmt *stmt);
    bool collectLocalVariables(const Stmt *body, std::set<LocalVariable> &variables);
    void collectFunctionParameters(const FunctionDecl *funcDecl, std::vector<FunctionParameter> &parameters);
    void collectMemberFunctionInfo(const FunctionDecl *funcDecl, CoroutineInfo &coro);
    std::string replaceLastTemplateArgWithHandle(const std::string &returnType);
    void updateFunctionReturnType(const CoroutineInfo &coro);
    void updateLambdaReturnType(const CoroutineInfo &coro);
    void rewriteCoroutineBody(CoroutineInfo &coro);
    std::string getTransformedBodyText(const Stmt *bodyStmt, CoroutineBodyRewriter &bodyRewriter);
    std::string generateCoroImplStruct(const CoroutineInfo &coro);
    SourceLocation findStructInsertionPoint(const FunctionDecl *funcDecl);
    void replaceEntireBodyWithStateMachine(const CoroutineInfo &coro);
    void wrapBodyWithRunMethod(const CoroutineInfo &coro, CoroutineBodyRewriter &bodyRewriter);

public:
    CoroutineRewriter(Rewriter &rewr, const SourceManager &SM,
                     clang::DiagnosticsEngine &diags, const LangOptions &langOpts)
        : sourceManager(SM), rewriter(rewr), diagnosticsEngine(diags),
          langOptions(langOpts), astContext(nullptr) {}

    void setASTContext(ASTContext &ctx) {
        astContext = &ctx;
    }

    bool VisitFunctionDecl(FunctionDecl *funcDecl);
    bool VisitLambdaExpr(LambdaExpr *lambdaExpr);
    void performRewrites();

    const std::vector<CoroutineInfo> &getCoroutines() const {
        return coroutines;
    }
};

#endif // LLVM_REWRITE_GENERATORS_COROUTINEREWRITER_H
