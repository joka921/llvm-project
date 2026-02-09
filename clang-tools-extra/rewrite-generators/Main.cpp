//
// Main entry point for the coroutine rewriter tool
//

#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "Common.h"
#include "CoroutineRewriter.cpp"

#include <memory>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Command-line options (declared as extern, defined in RewriteGenerators.cpp)
extern llvm::cl::OptionCategory MyToolCategory;

class MyASTConsumer : public ASTConsumer {
private:
    CoroutineRewriter &rewriter;

public:
    MyASTConsumer(CoroutineRewriter &rewr) : rewriter(rewr) {
    }

    void HandleTranslationUnit(ASTContext &Context) override {
        rewriter.setASTContext(Context);
        rewriter.TraverseDecl(Context.getTranslationUnitDecl());
    }
};

class CoroutineRewriterFrontendAction : public ASTFrontendAction {
private:
    std::unique_ptr<Rewriter> rewriter;
    SourceManager *sourceManager;
    std::unique_ptr<CoroutineRewriter> coroutineRewriter;

public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        sourceManager = &CI.getSourceManager();
        rewriter = std::make_unique<Rewriter>();
        rewriter->setSourceMgr(*sourceManager, CI.getLangOpts());

        coroutineRewriter = std::make_unique<CoroutineRewriter>(
            *rewriter, *sourceManager, CI.getDiagnostics(), CI.getLangOpts());

        return std::make_unique<MyASTConsumer>(*coroutineRewriter);
    }

    void EndSourceFileAction() override {
        coroutineRewriter->performRewrites();

        const auto &coroutines = coroutineRewriter->getCoroutines();
        REWRITE_LOG() << "\nSummary: Processed " << coroutines.size() << " coroutine(s)\n";

        SourceLocation mainFileLoc = sourceManager->getLocForStartOfFile(sourceManager->getMainFileID());
        const std::string filePath = sourceManager->getFilename(mainFileLoc).str();

        if (rewriter->getRewriteBufferFor(sourceManager->getMainFileID())) {
            const RewriteBuffer &RewriteBuf = rewriter->getEditBuffer(sourceManager->getMainFileID());

            std::error_code EC;
            llvm::raw_fd_ostream OS(filePath, EC, llvm::sys::fs::OF_Text);
            if (EC) {
                llvm::errs() << "Error opening file for writing: " << EC.message() << "\n";
                return;
            }

            RewriteBuf.write(OS);
            REWRITE_LOG() << "Rewrote file: " << filePath << "\n";
        } else {
            REWRITE_LOG() << "No changes needed for: " << filePath << "\n";
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
