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
#include <map>
#include <set>
#include <vector>

using namespace clang;

class CoroutineBodyRewriter;

struct TemplateInstantiationInfo {
    const FunctionDecl *instantiatedDecl;
    const FunctionDecl *patternDecl;
    const FunctionTemplateDecl *templateDecl;
};

class CoroutineRewriter : public RecursiveASTVisitor<CoroutineRewriter> {
private:
    const SourceManager &sourceManager;
    Rewriter &rewriter;
    clang::DiagnosticsEngine &diagnosticsEngine;
    const LangOptions &langOptions;
    ASTContext *astContext;
    std::vector<CoroutineInfo> coroutines;
    std::vector<TemplateInstantiationInfo> templateInstantiations;
    std::set<const FunctionTemplateDecl *> templateCoroutineDefinitions;

    // Lambda coroutines queued for extraction (non-generic)
    std::vector<CoroutineInfo> lambdaCoroutines;

    // Generic lambda coroutines — maps lambda closure type to extraction info
    struct GenericLambdaInfo {
        const LambdaExpr *lambdaExpr;
        std::string extractedName;
        bool isFileScopeLambda;
        const FunctionDecl *enclosingFunction;
        std::vector<TemplateInstantiationInfo> instantiations;
    };
    std::vector<GenericLambdaInfo> genericLambdaCoroutines;

    // Unmatched generic lambda instantiations (collected during VisitFunctionDecl,
    // matched to GenericLambdaInfo entries during extractGenericLambdaCoroutines)
    struct UnmatchedLambdaInstantiation {
        const FunctionDecl *instantiatedDecl;
        const FunctionDecl *patternDecl;
        const FunctionTemplateDecl *templateDecl;
        const CXXRecordDecl *closureType;
    };
    std::vector<UnmatchedLambdaInstantiation> unmatchedLambdaInstantiations;

    // Private helper methods
    bool containsCoroutineKeywords(const Stmt *stmt);
    bool containsTryCatchBlocks(const Stmt *stmt);
    bool collectLocalVariables(const Stmt *body, std::set<LocalVariable> &variables,
                               bool useDiagEngine = true);
    void collectFunctionParameters(const FunctionDecl *funcDecl, std::vector<FunctionParameter> &parameters);
    void collectMemberFunctionInfo(const FunctionDecl *funcDecl, CoroutineInfo &coro);
    std::string replaceLastTemplateArgWithHandle(const std::string &returnType);
    void updateFunctionReturnType(const CoroutineInfo &coro);
    void updateLambdaReturnType(const CoroutineInfo &coro);
    void rewriteCoroutineBody(CoroutineInfo &coro);
    std::string getTransformedBodyText(const Stmt *bodyStmt, CoroutineBodyRewriter &bodyRewriter);
    std::string generateCoroImplStruct(const CoroutineInfo &coro);
    SourceLocation findStructInsertionPoint(const FunctionDecl *funcDecl);
    SourceLocation findPreFunctionInsertionPoint(const FunctionDecl *funcDecl);
    void replaceEntireBodyWithStateMachine(const CoroutineInfo &coro);
    void wrapBodyWithRunMethod(const CoroutineInfo &coro, CoroutineBodyRewriter &bodyRewriter);

    // Template instantiation rewriting
    void rewriteTemplateInstantiations();
    void rewriteSingleTemplateInstantiation(const TemplateInstantiationInfo &info,
                                            std::string &outputText);
    std::string buildInstantiatedSignature(const FunctionDecl *funcDecl);
    SourceLocation findInsertionPointAfterIncludes();

    // #ifdef / #ifndef guards for template coroutines
    void wrapTemplateDefinitionsWithIfndef();
    std::string buildTemplateForwardDeclaration(const FunctionTemplateDecl *tmplDecl);

    // Lambda coroutine extraction
    void extractLambdaCoroutines();
    void extractSingleLambdaCoroutine(CoroutineInfo &coro);
    void extractGenericLambdaCoroutines();
    void extractSingleGenericLambda(GenericLambdaInfo &info);
    std::string generateLambdaFunctionName(const LambdaExpr *lambda);
    std::string buildLambdaFunctionSignature(const CoroutineInfo &coro);
    std::string buildGenericLambdaStructDeclaration(const GenericLambdaInfo &info);
    std::string buildGenericLambdaSpecializationSignature(const FunctionDecl *funcDecl,
                                                          const std::string &structName);
    bool isFileScopeLambda(const LambdaExpr *lambda);
    const FunctionDecl *findEnclosingFunction(const LambdaExpr *lambda);

public:
    CoroutineRewriter(Rewriter &rewr, const SourceManager &SM,
                     clang::DiagnosticsEngine &diags, const LangOptions &langOpts)
        : sourceManager(SM), rewriter(rewr), diagnosticsEngine(diags),
          langOptions(langOpts), astContext(nullptr) {}

    void setASTContext(ASTContext &ctx) {
        astContext = &ctx;
    }

    bool shouldVisitTemplateInstantiations() const { return true; }

    bool VisitFunctionDecl(FunctionDecl *funcDecl);
    bool VisitLambdaExpr(LambdaExpr *lambdaExpr);
    void performRewrites();

    const std::vector<CoroutineInfo> &getCoroutines() const {
        return coroutines;
    }
};

#endif // LLVM_REWRITE_GENERATORS_COROUTINEREWRITER_H
