//
// CoroutineBodyRewriter - Rewrites coroutine function bodies
//

#ifndef LLVM_REWRITE_GENERATORS_COROUTINEBODYREWRITER_H
#define LLVM_REWRITE_GENERATORS_COROUTINEBODYREWRITER_H

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Basic/SourceManager.h"

#include "Common.h"
#include "structures/CoroutineStructures.h"
#include "structures/LoopStructures.h"
#include "structures/TryCatchStructures.h"
#include "structures/ScopeStructures.h"

#include <map>
#include <set>
#include <vector>
#include <optional>

using namespace clang;

class CoroutineBodyRewriter : public RecursiveASTVisitor<CoroutineBodyRewriter> {
private:
    const std::set<LocalVariable> &localVariables;
    Rewriter &rewriter;
    const SourceManager &sourceManager;
    CoroutineInfo &coroutineInfo;
    ASTContext *astContext;
    std::set<std::string> variableNames;
    std::vector<Replacement> declReplacements;
    std::vector<Replacement> refReplacements;
    std::set<SourceLocation> processedDeclarations;
    std::set<SourceLocation> processedThisExpressions;
    std::vector<ScopeInfo> scopeStack;
    std::vector<const Stmt*> currentLoopStack;
    std::vector<std::pair<SourceLocation, std::string> > destructorInsertions;
    std::vector<CoroutineStatement> coroutineStatements;
    unsigned nextCoroStatementIndex;
    std::vector<RangedForLoop> rangedForLoops;
    unsigned nextRangedForIndex;
    std::vector<TryCatchBlock> tryCatchBlocks;
    unsigned nextTryCatchIndex;
    std::vector<unsigned> currentTryBlockStack;

    bool isMemberFunction;
    [[maybe_unused]] const CXXRecordDecl *classDecl;
    std::map<SourceLocation, std::string> declLocationToMemberName;

    enum InitializationForm {
        CONSTRUCT_CALL,
        BRACED_INIT,
        PAREN_INIT
    };

    // Private helper methods
    void buildDeclLocationMapping();
    SourceLocation getLocForEndOfToken(SourceLocation loc);
    InitializationForm getInitializationForm(const VarDecl *varDecl);
    std::optional<SourceRange> handleParenthesizedInitialization(const VarDecl *varDecl);
    SourceLocation findClosingParen(SourceLocation startLoc);
    void processParenthesizedArguments(const VarDecl *varDecl);
    std::optional<SourceRange> handleBracedInitialization(const VarDecl *varDecl);
    void insertDestructorsForScope(const ScopeInfo &scope);
    void insertLoopVariableDestructors(const Stmt *stmt, bool isBreak);
    std::string transformCatchClauseBody(const CXXCatchStmt *catchStmt, const std::string &exceptionVarName);
    void collectCoroutineStatementReplacements();
    void collectRangeReplacements();
    void collectRangedForLoopReplacements();
    void collectTryCatchReplacements();
    void collectScopeEndReplacements();
    void applyAllReplacements();
    void collectTemporariesFromExpression(const Expr *expr, CoroutineStatement &coroStmt);
    std::string processCatchClause(const CXXCatchStmt *catchStmt);
    bool isPartOfDeclaration(const DeclRefExpr *declRef);
    std::string rewriteVariableReferencesInText(const std::string &text, const std::string &varName);
    std::string processInitExpression(const Expr *init, const std::string &currentVarName);
    std::string handleConstructorArgs(const CXXConstructExpr *constructExpr);
    std::string handleInitListArgs(const InitListExpr *initList);
    std::string rewriteExpression(const Expr *expr);
    std::string rewriteExpressionExceptVar(const Expr *expr, const std::string &excludeVar);
    std::string generateExplicitLoopForm(const RangedForLoop &rangedFor);
    std::string transformVariableReferencesInText(const std::string &text);
    std::string getInitializationArguments(const VarDecl *varDecl);
    void collectDestructorInsertions();

public:
    std::map<unsigned, std::vector<ScopeEndReplacement> > scopeEndReplacements;
    std::vector<std::tuple<SourceRange, std::string, int, bool> > globalReplacements;

    CoroutineBodyRewriter(const std::set<LocalVariable> &vars, Rewriter &rewr, const SourceManager &SM,
                          CoroutineInfo &coroInfo, ASTContext &astCtx, bool isMember = false,
                          const CXXRecordDecl *classRecord = nullptr);

    // AST visitor methods
    bool TraverseCompoundStmt(CompoundStmt *compoundStmt);
    bool VisitDeclStmt(DeclStmt *declStmt);
    bool VisitCoyieldExpr(CoyieldExpr *coyield);
    bool VisitCoawaitExpr(CoawaitExpr *coawait);
    bool VisitCoreturnStmt(CoreturnStmt *coreturn);
    bool TraverseCXXTryStmt(CXXTryStmt *tryStmt);
    bool TraverseCXXForRangeStmt(CXXForRangeStmt *forRange);
    bool VisitDeclRefExpr(DeclRefExpr *declRef);
    bool VisitMemberExpr(MemberExpr *memberExpr);
    bool VisitCXXThisExpr(CXXThisExpr *thisExpr);
    bool TraverseForStmt(ForStmt *forStmt);
    bool TraverseWhileStmt(WhileStmt *whileStmt);
    bool TraverseDoStmt(DoStmt *doStmt);
    bool VisitBreakStmt(BreakStmt *breakStmt);
    bool VisitContinueStmt(ContinueStmt *continueStmt);

    // Replacement application
    void applyReplacements();

    // Getters
    const std::vector<RangedForLoop> &getRangedForLoops() const {
        return rangedForLoops;
    }

    const std::vector<TryCatchBlock> &getTryCatchBlocks() const {
        return tryCatchBlocks;
    }

    const std::vector<CoroutineStatement> &getCoroutineStatements() const {
        return coroutineStatements;
    }
};

#endif // LLVM_REWRITE_GENERATORS_COROUTINEBODYREWRITER_H
