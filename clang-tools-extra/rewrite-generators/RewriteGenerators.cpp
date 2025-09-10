//
// Created by kalmbacj on 2025-09-09.
//

#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/AST/Stmt.h"
#include "clang/Lex/Lexer.h"

#include "llvm/Support/CommandLine.h"
#include <iostream>
#include <set>

using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

static llvm::cl::OptionCategory MyToolCategory("coroutine-rewriter");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("\nRewrites C++20 coroutines to C++17 compatible state machines.\n");

struct LocalVariable {
    std::string name;
    std::string type;
    SourceLocation location;
    
    bool operator<(const LocalVariable& other) const {
        return name < other.name;
    }
};

struct CoroutineInfo {
    const FunctionDecl* function;
    std::set<LocalVariable> localVariables;
    SourceLocation insertionPoint;
    bool hasError = false;
};

class LocalVariableCollector : public RecursiveASTVisitor<LocalVariableCollector> {
private:
    std::set<LocalVariable>& variables;
    const SourceManager& sourceManager;
    ASTContext& astContext;

public:
    LocalVariableCollector(std::set<LocalVariable>& vars, const SourceManager& SM, ASTContext& ctx) 
        : variables(vars), sourceManager(SM), astContext(ctx) {}

    std::string getFullyQualifiedTypeName(QualType type) {
        // Get the canonical type (resolves typedefs, auto, etc.)
        QualType canonicalType = type.getCanonicalType();
        
        // Use a printing policy that produces fully qualified names
        PrintingPolicy policy(astContext.getLangOpts());
        policy.SuppressScope = false;  // Include scope information
        policy.SuppressTagKeyword = false;  // Keep 'struct', 'class', etc.
        policy.SuppressUnwrittenScope = false;  // Include all scopes
        policy.FullyQualifiedName = true;  // Force fully qualified names
        policy.PrintCanonicalTypes = true;  // Print canonical types
        
        return canonicalType.getAsString(policy);
    }

    bool VisitDeclStmt(DeclStmt* declStmt) {
        for (auto* decl : declStmt->decls()) {
            if (auto* varDecl = dyn_cast<VarDecl>(decl)) {
                if (!varDecl->getType()->isFunctionType() && 
                    !varDecl->getType()->isReferenceType()) {
                    LocalVariable var;
                    var.name = varDecl->getNameAsString();
                    var.type = getFullyQualifiedTypeName(varDecl->getType());
                    var.location = varDecl->getLocation();
                    
                    if (!var.name.empty()) {
                        variables.insert(var);
                        std::cout << "  Found local variable: " << var.type << " " << var.name << "\n";
                    }
                }
            }
        }
        return true;
    }
};

class CoroutineRewriter : public RecursiveASTVisitor<CoroutineRewriter> {
private:
    const SourceManager& sourceManager;
    Rewriter& rewriter;
    clang::DiagnosticsEngine& diagnosticsEngine;
    const LangOptions& langOptions;
    ASTContext* astContext;
    std::vector<CoroutineInfo> coroutines;

    bool containsCoroutineKeywords(const Stmt* stmt) {
        if (!stmt) return false;
        
        if (isa<CoawaitExpr>(stmt) || isa<CoyieldExpr>(stmt) || isa<CoreturnStmt>(stmt)) {
            return true;
        }
        
        for (auto it = stmt->child_begin(); it != stmt->child_end(); ++it) {
            if (const Stmt* child = *it) {
                if (containsCoroutineKeywords(child)) {
                    return true;
                }
            }
        }
        return false;
    }

    void collectLocalVariables(const Stmt* body, std::set<LocalVariable>& variables) {
        LocalVariableCollector collector(variables, sourceManager, *astContext);
        collector.TraverseStmt(const_cast<Stmt*>(body));
    }

    SourceLocation findStructInsertionPoint(const FunctionDecl* funcDecl) {
        std::cout << "DEBUG: findStructInsertionPoint called for function: " << funcDecl->getQualifiedNameAsString() << "\n";
        
        const Stmt* bodyStmt = funcDecl->getBody();
        if (!bodyStmt) {
            std::cout << "DEBUG: Function has no body\n";
            return SourceLocation();
        }
        
        std::cout << "DEBUG: Function has body, type: " << bodyStmt->getStmtClassName() << "\n";
        
        // Handle CoroutineBodyStmt wrapper
        if (auto* coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            std::cout << "DEBUG: Body is CoroutineBodyStmt, getting inner body\n";
            bodyStmt = coroBody->getBody();
            if (!bodyStmt) {
                std::cout << "DEBUG: CoroutineBodyStmt has no inner body\n";
                return SourceLocation();
            }
            std::cout << "DEBUG: Inner body type: " << bodyStmt->getStmtClassName() << "\n";
        }
        
        if (auto* body = dyn_cast<CompoundStmt>(bodyStmt)) {
            std::cout << "DEBUG: Body is CompoundStmt\n";
            SourceLocation lbracLoc = body->getLBracLoc();
            std::cout << "DEBUG: getLBracLoc() returned location, checking validity\n";
            
            if (lbracLoc.isValid()) {
                std::cout << "DEBUG: lbracLoc is valid, calling Lexer::getLocForEndOfToken\n";
                SourceLocation endLoc = Lexer::getLocForEndOfToken(lbracLoc, 0, sourceManager, langOptions);
                if (endLoc.isValid()) {
                    std::cout << "DEBUG: Successfully found insertion point\n";
                    return endLoc;
                } else {
                    std::cout << "DEBUG: Lexer::getLocForEndOfToken returned invalid location\n";
                }
            } else {
                std::cout << "DEBUG: lbracLoc is invalid\n";
            }
        } else {
            std::cout << "DEBUG: Final body is not CompoundStmt, type: " << bodyStmt->getStmtClassName() << "\n";
        }
        
        std::cout << "DEBUG: Returning invalid SourceLocation\n";
        return SourceLocation();
    }

    std::string generateCoroImplStruct(const CoroutineInfo& coro) {
        std::string structCode = "\n  struct _detail_coro_impl {\n";
        
        if (coro.localVariables.empty()) {
            structCode += "    // No local variables found in this coroutine\n";
        } else {
            for (const auto& var : coro.localVariables) {
                structCode += "    " + var.type + " " + var.name + "{};\n";
            }
        }
        
        structCode += "  } _coro_state;\n\n";
        return structCode;
    }

public:
    CoroutineRewriter(Rewriter& rewr, const SourceManager& SM, DiagnosticsEngine& diag, const LangOptions& langOpts)
        : sourceManager(SM), rewriter(rewr), diagnosticsEngine(diag), langOptions(langOpts), astContext(nullptr) {}

    void setASTContext(ASTContext& ctx) {
        astContext = &ctx;
    }

    bool VisitFunctionDecl(FunctionDecl* funcDecl) {
        if (!funcDecl->hasBody()) {
            return true;
        }

        auto fileId = sourceManager.getFileID(funcDecl->getLocation());
        if (fileId != sourceManager.getMainFileID()) {
            return true;
        }

        if (containsCoroutineKeywords(funcDecl->getBody())) {
            CoroutineInfo coro;
            coro.function = funcDecl;
            
            std::cout << "Processing coroutine: " << funcDecl->getQualifiedNameAsString() << "\n";
            
            collectLocalVariables(funcDecl->getBody(), coro.localVariables);
            
            coro.insertionPoint = findStructInsertionPoint(funcDecl);
            if (coro.insertionPoint.isInvalid()) {
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Warning,
                    "Could not find insertion point for _detail_coro_impl struct");
                diagnosticsEngine.Report(funcDecl->getLocation(), diagID);
                coro.hasError = true;
                
                // Output the struct code to console as fallback
                std::string structCode = generateCoroImplStruct(coro);
                std::cout << "WARNING: Could not insert struct, here's what would have been inserted:\n";
                std::cout << "========== STRUCT CODE ==========\n";
                std::cout << structCode;
                std::cout << "==================================\n";
            }
            
            coroutines.push_back(coro);
            
            std::cout << "  Found " << coro.localVariables.size() << " local variables\n";
            
            if (const auto* methodDecl = dyn_cast<CXXMethodDecl>(funcDecl)) {
                std::cout << "  Type: Member function of " << methodDecl->getParent()->getQualifiedNameAsString() << "\n";
            } else {
                std::cout << "  Type: Free function\n";
            }
        }

        return true;
    }

    void performRewrites() {
        for (const auto& coro : coroutines) {
            if (coro.hasError) {
                continue;
            }
            
            std::string structCode = generateCoroImplStruct(coro);
            
            if (coro.insertionPoint.isValid()) {
                rewriter.InsertTextBefore(coro.insertionPoint, structCode);
                std::cout << "Inserted _detail_coro_impl struct into " 
                         << coro.function->getQualifiedNameAsString() << "\n";
            }
        }
    }

    const std::vector<CoroutineInfo>& getCoroutines() const {
        return coroutines;
    }
};

class MyASTConsumer : public ASTConsumer {
private:
    CoroutineRewriter& rewriter;

public:
    MyASTConsumer(CoroutineRewriter& rewr) : rewriter(rewr) {}

    void HandleTranslationUnit(ASTContext& Context) override {
        rewriter.setASTContext(Context);
        rewriter.TraverseDecl(Context.getTranslationUnitDecl());
    }
};

class CoroutineRewriterFrontendAction : public ASTFrontendAction {
private:
    std::unique_ptr<Rewriter> rewriter;
    SourceManager* sourceManager;
    std::unique_ptr<CoroutineRewriter> coroutineRewriter;

public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& CI, llvm::StringRef InFile) override {
        
        sourceManager = &CI.getSourceManager();
        rewriter = std::make_unique<Rewriter>();
        rewriter->setSourceMgr(*sourceManager, CI.getLangOpts());
        
        coroutineRewriter = std::make_unique<CoroutineRewriter>(
            *rewriter, *sourceManager, CI.getDiagnostics(), CI.getLangOpts());
        
        return std::make_unique<MyASTConsumer>(*coroutineRewriter);
    }

    void EndSourceFileAction() override {
        coroutineRewriter->performRewrites();
        
        const auto& coroutines = coroutineRewriter->getCoroutines();
        std::cout << "\nSummary: Processed " << coroutines.size() << " coroutine(s)\n";
        
        SourceLocation mainFileLoc = sourceManager->getLocForStartOfFile(sourceManager->getMainFileID());
        const std::string filePath = sourceManager->getFilename(mainFileLoc).str();
        
        if (rewriter->getRewriteBufferFor(sourceManager->getMainFileID())) {
            const RewriteBuffer& RewriteBuf = rewriter->getEditBuffer(sourceManager->getMainFileID());
            
            std::error_code EC;
            llvm::raw_fd_ostream OS(filePath, EC, llvm::sys::fs::OF_Text);
            if (EC) {
                llvm::errs() << "Error opening file for writing: " << EC.message() << "\n";
                return;
            }
            
            RewriteBuf.write(OS);
            std::cout << "Rewrote file: " << filePath << "\n";
        } else {
            std::cout << "No changes needed for: " << filePath << "\n";
        }
    }
};

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) {
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());
    
    auto tool = newFrontendActionFactory<CoroutineRewriterFrontendAction>();
    int result = Tool.run(tool.get());
    return result;
}