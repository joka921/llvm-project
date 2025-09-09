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

#include "llvm/Support/CommandLine.h"
#include <iostream>

using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

static llvm::cl::OptionCategory MyToolCategory("coroutine-detector");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("\nDetects C++20 coroutines in source code.\n");

class CoroutineDetector : public RecursiveASTVisitor<CoroutineDetector> {
private:
    const SourceManager &sourceManager;
    std::vector<const FunctionDecl*> coroutineFunctions;

    bool containsCoroutineKeywords(const Stmt* body) {
        if (!body) return false;
        
        for (auto it = body->child_begin(); it != body->child_end(); ++it) {
            if (const Stmt* child = *it) {
                if (isa<CoawaitExpr>(child) || isa<CoyieldExpr>(child) || isa<CoreturnStmt>(child)) {
                    return true;
                }
                if (containsCoroutineKeywords(child)) {
                    return true;
                }
            }
        }
        return false;
    }

public:
    explicit CoroutineDetector(const SourceManager &SM) : sourceManager(SM) {}

    bool VisitFunctionDecl(FunctionDecl *funcDecl) {
        if (!funcDecl->hasBody()) {
            return true;
        }

        auto fileId = sourceManager.getFileID(funcDecl->getLocation());
        if (fileId != sourceManager.getMainFileID()) {
            return true;
        }

        if (containsCoroutineKeywords(funcDecl->getBody())) {
            coroutineFunctions.push_back(funcDecl);
            
            std::cout << "Found coroutine function: " << funcDecl->getQualifiedNameAsString() << "\n";
            std::cout << "  Location: " << funcDecl->getLocation().printToString(sourceManager) << "\n";
            std::cout << "  Return type: " << funcDecl->getReturnType().getAsString() << "\n";
            
            if (const auto* methodDecl = dyn_cast<CXXMethodDecl>(funcDecl)) {
                std::cout << "  Type: Member function\n";
                std::cout << "  Class: " << methodDecl->getParent()->getQualifiedNameAsString() << "\n";
            } else {
                std::cout << "  Type: Free function\n";
            }
            
            std::cout << "  Parameters: ";
            for (unsigned i = 0; i < funcDecl->getNumParams(); ++i) {
                if (i > 0) std::cout << ", ";
                const auto* param = funcDecl->getParamDecl(i);
                std::cout << param->getType().getAsString();
                if (!param->getNameAsString().empty()) {
                    std::cout << " " << param->getNameAsString();
                }
            }
            if (funcDecl->getNumParams() == 0) {
                std::cout << "none";
            }
            std::cout << "\n\n";
        }

        return true;
    }

    const std::vector<const FunctionDecl*>& getCoroutineFunctions() const {
        return coroutineFunctions;
    }
};

class MyASTConsumer : public ASTConsumer {
private:
    CoroutineDetector detector;

public:
    MyASTConsumer(const SourceManager &SM) : detector(SM) {}

    void HandleTranslationUnit(ASTContext &Context) override {
        detector.TraverseDecl(Context.getTranslationUnitDecl());
        
        const auto& coroutines = detector.getCoroutineFunctions();
        std::cout << "Summary: Found " << coroutines.size() << " coroutine function(s)\n";
    }
};

class CoroutineDetectorFrontendAction : public ASTFrontendAction {
public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        return std::make_unique<MyASTConsumer>(CI.getSourceManager());
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
    
    auto tool = newFrontendActionFactory<CoroutineDetectorFrontendAction>();
    int result = Tool.run(tool.get());
    return result;
}