//
// Created by kalmbacj on 3/10/25.
//

// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"

// Declares llvm::cl::extrahelp.
#include "llvm/Support/CommandLine.h"
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticIDs.h>

using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("cxx-20-to-17-rewriters");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterward.
static cl::extrahelp MoreHelp("\nMore help text...\n");

// A matcher that matches calls to starts_with member function with exactly one argument
static StatementMatcher StartsWithMatcher = cxxMemberCallExpr(
    callee(cxxMethodDecl(hasName("starts_with"))),
    argumentCountIs(1)
).bind("starts-with-call");

// This class collects all the matching starts_with calls and rewrites them.
class StartsWithRewriter : public MatchFinder::MatchCallback {
    // Structure to store information about a starts_with call
    struct StartsWithInfo {
        SourceRange sourceRange;
        std::string objectExpr;
        std::string argument;
        bool hasError = false;
    };

    std::vector<StartsWithInfo> matches;

    Rewriter &rewriter;
    const SourceManager &sourceManager;
    clang::DiagnosticsEngine &diagnosticsEngine;

public:
    StartsWithRewriter(Rewriter &rewr, const SourceManager &manager, DiagnosticsEngine &engine) 
        : rewriter(rewr), sourceManager(manager), diagnosticsEngine(engine) {
    }

    // Helper function to get the source text for an expression
    std::string getSourceText(const Expr *expr, const SourceManager &SM, const LangOptions &LO) {
        return Lexer::getSourceText(CharSourceRange::getTokenRange(expr->getSourceRange()), SM, LO).str();
    }

    // This method is called for each starts_with call that matches the matcher.
    void run(const MatchFinder::MatchResult &Result) override {
        const CXXMemberCallExpr *CallExpr = Result.Nodes.getNodeAs<clang::CXXMemberCallExpr>("starts-with-call");

        // Don't rewrite calls that are contained in included header files, only rewrite the currently active file.
        auto fileId = sourceManager.getFileID(CallExpr->getBeginLoc());
        if (fileId != sourceManager.getMainFileID()) {
            return;
        }

        // Create a new match entry
        matches.emplace_back();
        StartsWithInfo &info = matches.back();
        info.sourceRange = CallExpr->getSourceRange();

        // Get the object on which starts_with is called
        const Expr *objectExpr = CallExpr->getImplicitObjectArgument();
        if (!objectExpr) {
            unsigned diagID = diagnosticsEngine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                                "Cannot determine object for starts_with call");
            info.hasError = true;
            diagnosticsEngine.Report(sourceManager.getFileLoc(CallExpr->getBeginLoc()), diagID);
            return;
        }

        // Get the source text for the object
        info.objectExpr = getSourceText(objectExpr, sourceManager, Result.Context->getLangOpts());

        // Get the argument to starts_with
        if (CallExpr->getNumArgs() != 1) {
            unsigned diagID = diagnosticsEngine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                                "starts_with call must have exactly one argument");
            info.hasError = true;
            diagnosticsEngine.Report(sourceManager.getFileLoc(CallExpr->getBeginLoc()), diagID);
            return;
        }

        const Expr *argExpr = CallExpr->getArg(0);
        info.argument = getSourceText(argExpr, sourceManager, Result.Context->getLangOpts());
    }

    // Generate the replacement text for a starts_with call
    std::string generateReplacement(const StartsWithInfo &info) {
        return "ql::starts_with(" + info.objectExpr + ", " + info.argument + ")";
    }

    // This function performs the actual rewrite.
    void rewrite() {
        for (const auto &info : matches) {
            if (info.hasError) {
                continue;
            }
            
            std::string replacement = generateReplacement(info);
            rewriter.ReplaceText(info.sourceRange, replacement);
        }
    }
};

class MyASTConsumer : public ASTConsumer {
public:
    MyASTConsumer(StartsWithRewriter &callback) {
        Finder.addMatcher(StartsWithMatcher, &callback);
    }

    void HandleTranslationUnit(ASTContext &Context) override {
        // Perform matching
        Finder.matchAST(Context);
    }

private:
    MatchFinder Finder;
};

// This class handles inputs on a per-file base.
class RewriteStartsWithFrontendAction : public ASTFrontendAction {
    std::unique_ptr<Rewriter> Rewrite = std::make_unique<Rewriter>();
    const SourceManager *SM;
    std::unique_ptr<StartsWithRewriter> startsWithRewriter;

public:
    // This function sets up the StartsWithRewriter.
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        // Initialize the Rewriter with the SourceManager from the CompilerInstance
        SM = &CI.getSourceManager();
        startsWithRewriter = std::make_unique<StartsWithRewriter>(*Rewrite, *SM, CI.getDiagnostics());
        // Set the rewriter's buffer to the file being processed
        Rewrite->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

        // Set up the callback for replacing text in the source code
        return std::make_unique<MyASTConsumer>(*startsWithRewriter);
    }

    bool BeginSourceFileAction(CompilerInstance &CI) override {
        // Make sure the rewriter is initialized
        return true;
    }

    // Save the modified source code back to the file
    void EndSourceFileAction() override {
        startsWithRewriter->rewrite();
        // Write the modified content back to the original file

        // Get the location of the main file (source file)
        SourceLocation mainFileLoc = SM->getLocForStartOfFile(SM->getMainFileID());

        // Get the file name from the source location
        const std::string filePath = SM->getFilename(mainFileLoc).str();

        // Get the edit buffer that holds the modified source (it has been filled by the call to `rewrite` above.
        const RewriteBuffer &RewriteBuf = Rewrite->getEditBuffer(SM->getMainFileID());

        // Open the file for writing
        std::error_code EC;
        llvm::raw_fd_ostream OS(filePath, EC, llvm::sys::fs::OF_Text);
        if (EC) {
            llvm::errs() << "Error opening file for writing: " << EC.message() << "\n";
            return;
        }

        // Write the modified content to the file
        RewriteBuf.write(OS);
    }
};

// Main function that sets up everything and runs the file.
int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
    if (!ExpectedParser) {
        // Fail gracefully for unsupported options.
        llvm::errs() << ExpectedParser.takeError();
        return 1;
    }
    CommonOptionsParser &OptionsParser = ExpectedParser.get();
    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());
    auto tool = newFrontendActionFactory<RewriteStartsWithFrontendAction>();
    int result = Tool.run(tool.get());
    return result;
}