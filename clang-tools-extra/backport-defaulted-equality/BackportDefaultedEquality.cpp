//
// Created by kalmbacj on 3/10/25.
//

// Declares clang::SyntaxOnlyAction.
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"

// Declares llvm::cl::extrahelp.
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;


// Apply a custom category to all command-line options so that they are the
// only ones displayed.
static llvm::cl::OptionCategory MyToolCategory("my-tool options");

// CommonOptionsParser declares HelpMessage with a description of the common
// command-line options related to the compilation database and input files.
// It's nice to have this help message in all tools.
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);

// A help message for this specific tool can be added afterwards.
static cl::extrahelp MoreHelp("\nMore help text...\n");

static StatementMatcher LoopMatcher =
        forStmt(hasLoopInit(declStmt(hasSingleDecl(varDecl(
            hasInitializer(integerLiteral(equals(0)))))))).bind("forLoop");

static DeclarationMatcher ClassMatcher = cxxRecordDecl(allOf(forEach(fieldDecl().bind("field")),
                                                             hasMethod(cxxMethodDecl(
                                                                 allOf(hasName("operator=="), isDefaulted())).bind(
                                                                 "method"))
        ))
        .
        bind(
            "record"
        );

class LoopPrinter : public MatchFinder::MatchCallback {
public :
    void run(const MatchFinder::MatchResult &Result) override {
        if (const ForStmt *FS = Result.Nodes.getNodeAs<clang::ForStmt>("forLoop"))
            FS->dump();
    }
};


class DeclPrinter : public MatchFinder::MatchCallback {
    struct Res {
        SourceRange operatorRanges;
        std::vector<std::string> recordNames;
        std::vector<std::string> fieldNames;
    };
    SourceRange activeSourceRange;
    std::vector<Res> matches;

    Rewriter& rewriter;
public :
    DeclPrinter(Rewriter& rewr) : rewriter(rewr) {}
    void run(const MatchFinder::MatchResult &Result) override {
        //if (const RecordDecl *FS = Result.Nodes.getNodeAs<clang::RecordDecl>("record"))
        //   FS->dump();

        const CXXRecordDecl *Decl = Result.Nodes.getNodeAs<clang::CXXRecordDecl>("record");
        auto rng = Decl->getSourceRange();
        if (rng != activeSourceRange) {
            activeSourceRange = rng;
            matches.emplace_back();
            matches.back().recordNames.push_back(Decl->getNameAsString());
        }
        auto &m = matches.back();

        if (const CXXMethodDecl *Decl = Result.Nodes.getNodeAs<clang::CXXMethodDecl>("method")) {
            m.operatorRanges = Decl->getSourceRange();
            //Decl->dump();
        }
        if (const FieldDecl *Field = Result.Nodes.getNodeAs<clang::FieldDecl>("field")) {
            m.fieldNames.push_back(Field->getNameAsString());
            //Field->dump();
        }
    }

    void printMembers() const {
        for (const auto& m : matches) {
            llvm::outs() << "Printing a struct of name " << m.recordNames.at(0);
            for (const auto &s: m.fieldNames) {
                llvm::outs() << "\n  " << s;
            }
            llvm::outs() << '\n';
        }
    }

    void rewrite() {
      for (const auto& m : matches) {
          const auto& className = m.recordNames.at(0);
          std::string rewrite = "bool operator==(const " + className + "& otherRhs) const {\n";
          for (const auto& mem : m.fieldNames) {
              rewrite += "    if (" + mem + " != otherRhs." + mem + ") return false;\n";
          }
          rewrite += "    return true;\n  }";
          rewriter.ReplaceText(m.operatorRanges, rewrite);
      }
    }
};

class MyASTConsumer : public ASTConsumer {
public:
    using P = std::unique_ptr<Rewriter>;
    MyASTConsumer(P _Rewrite) : Rewrite(std::move(_Rewrite)) {
        Finder.addMatcher(ClassMatcher, &Callback);
    }

    void HandleTranslationUnit(ASTContext &Context) override {
        // Perform matching
        Finder.matchAST(Context);
    }

    ~MyASTConsumer() {
        //Callback.printMembers();
        Callback.rewrite();
        Rewrite->getEditBuffer(Rewrite->getSourceMgr().getMainFileID()).write (llvm::outs());
    }

private:
    P Rewrite;
    DeclPrinter Callback{*Rewrite};
    MatchFinder Finder;
};

class MyFrontendAction : public ASTFrontendAction {
public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        // Initialize the Rewriter with the SourceManager from the CompilerInstance
        auto Rewrite = std::make_unique<Rewriter>();

        // Set the rewriter's buffer to the file being processed
        Rewrite->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

        // Set up the callback for replacing text in the source code
        return std::make_unique<MyASTConsumer>(std::move(Rewrite));
    }
    // TODO<joka921> The following was suggested by ChatGPT to modify the files in place.
    /*
    bool BeginSourceFileAction(CompilerInstance &CI) override {
        // Make sure the rewriter is initialized
        return true;
    }

    // Save the modified source code back to the file
    void EndSourceFileAction() override {
        // Write the modified content back to the original file
        llvm::errs() << "Writing changes back to file...\n";

        // Get the file path from the compiler instance
        const FileID &FID = getCompilerInstance().getSourceManager().getMainFileID();
        const std::string filePath = getCompilerInstance().getSourceManager().getFilename(FID).str();

        // Get the edit buffer that holds the modified source
        const RewriteBuffer &RewriteBuf = Rewrite.getEditBuffer(FID);

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
    */
};

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
    auto tool = newFrontendActionFactory<MyFrontendAction>();
    int result =  Tool.run(tool.get());
    return result;
}
