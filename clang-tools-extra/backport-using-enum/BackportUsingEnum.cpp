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

// A matcher that matches using enum declarations
static DeclarationMatcher UsingEnumMatcher = usingEnumDecl().bind("using-enum");

// This class collects all the matching using enum declarations and rewrites them.
class UsingEnumRewriter : public MatchFinder::MatchCallback {
    // Structure to store information about a using enum declaration
    struct UsingEnumInfo {
        SourceRange sourceRange;
        std::string enumName;
        std::vector<std::string> enumerators;
        bool hasError = false;
    };

    std::vector<UsingEnumInfo> matches;

    Rewriter &rewriter;
    const SourceManager &sourceManager;
    clang::DiagnosticsEngine &diagnosticsEngine;

public:
    UsingEnumRewriter(Rewriter &rewr, const SourceManager &manager, DiagnosticsEngine &engine) 
        : rewriter(rewr), sourceManager(manager), diagnosticsEngine(engine) {
    }

    // This method is called for each using enum declaration that matches the matcher.
    void run(const MatchFinder::MatchResult &Result) override {
        const UsingEnumDecl *Decl = Result.Nodes.getNodeAs<clang::UsingEnumDecl>("using-enum");

        // Don't rewrite declarations that are contained in included header files, only rewrite the currently active file.
        auto fileId = sourceManager.getFileID(Decl->getLocation());
        if (fileId != sourceManager.getMainFileID()) {
            return;
        }

        // Create a new match entry
        matches.emplace_back();
        UsingEnumInfo &info = matches.back();
        
        // Extend the source range to include the semicolon
        SourceRange range = Decl->getSourceRange();
        SourceLocation endLoc = range.getEnd();
        
        // Find the semicolon after the declaration
        SourceLocation semiLoc = endLoc.getLocWithOffset(1);
        while (semiLoc.isValid()) {
            const char *semiPtr = sourceManager.getCharacterData(semiLoc);
            if (*semiPtr == ';') {
                endLoc = semiLoc;
                break;
            }
            if (*semiPtr == '\n' || *semiPtr == '\0') {
                break;
            }
            semiLoc = semiLoc.getLocWithOffset(1);
        }
        
        info.sourceRange = SourceRange(range.getBegin(), endLoc);

        // Get the enum declaration
        const EnumDecl *enumDecl = Decl->getEnumDecl();
        if (!enumDecl) {
            unsigned diagID = diagnosticsEngine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                                "Cannot resolve enum declaration for using enum");
            info.hasError = true;
            diagnosticsEngine.Report(sourceManager.getFileLoc(Decl->getSourceRange().getBegin()), diagID);
            return;
        }

        info.enumName = enumDecl->getQualifiedNameAsString();

        // Collect all enumerators from the enum
        for (const EnumConstantDecl *enumConstant : enumDecl->enumerators()) {
            info.enumerators.push_back(enumConstant->getNameAsString());
        }

        // Check if we found any enumerators
        if (info.enumerators.empty()) {
            unsigned diagID = diagnosticsEngine.getCustomDiagID(clang::DiagnosticsEngine::Warning,
                                                                "Using enum declaration for empty enum");
            diagnosticsEngine.Report(sourceManager.getFileLoc(Decl->getSourceRange().getBegin()), diagID);
        }
    }

    // Generate the replacement text for a using enum declaration
    std::string generateReplacement(const UsingEnumInfo &info) {
        std::string replacement;
        
        for (size_t i = 0; i < info.enumerators.size(); ++i) {
            replacement += "using " + info.enumName + "::" + info.enumerators[i];
            if (i < info.enumerators.size() - 1) {
                replacement += ";\n";
            } else {
                replacement += ";";
            }
        }
        
        return replacement;
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
    MyASTConsumer(UsingEnumRewriter &callback) {
        Finder.addMatcher(UsingEnumMatcher, &callback);
    }

    void HandleTranslationUnit(ASTContext &Context) override {
        // Perform matching
        Finder.matchAST(Context);
    }

private:
    MatchFinder Finder;
};

// This class handles inputs on a per-file base.
class RewriteUsingEnumFrontendAction : public ASTFrontendAction {
    std::unique_ptr<Rewriter> Rewrite = std::make_unique<Rewriter>();
    const SourceManager *SM;
    std::unique_ptr<UsingEnumRewriter> usingEnumRewriter;

public:
    // This function sets up the UsingEnumRewriter.
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        // Initialize the Rewriter with the SourceManager from the CompilerInstance
        SM = &CI.getSourceManager();
        usingEnumRewriter = std::make_unique<UsingEnumRewriter>(*Rewrite, *SM, CI.getDiagnostics());
        // Set the rewriter's buffer to the file being processed
        Rewrite->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

        // Set up the callback for replacing text in the source code
        return std::make_unique<MyASTConsumer>(*usingEnumRewriter);
    }

    bool BeginSourceFileAction(CompilerInstance &CI) override {
        // Make sure the rewriter is initialized
        return true;
    }

    // Save the modified source code back to the file
    void EndSourceFileAction() override {
        usingEnumRewriter->rewrite();
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
    auto tool = newFrontendActionFactory<RewriteUsingEnumFrontendAction>();
    int result = Tool.run(tool.get());
    return result;
}