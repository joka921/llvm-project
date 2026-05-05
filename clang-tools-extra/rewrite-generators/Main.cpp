//
// Main entry point for the coroutine rewriter tool
//

#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include "Common.h"
#include "CoroutineRewriter.h"

#include <memory>
#include <set>
#include <string>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

// Command-line options (declared as extern, defined in RewriteGenerators.cpp)
extern llvm::cl::OptionCategory MyToolCategory;
extern llvm::cl::opt<bool> Verbose;
extern llvm::cl::opt<bool> InPlace;
extern llvm::cl::opt<std::string> OutputFile;

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
    SourceManager *sourceManager = nullptr;
    std::unique_ptr<CoroutineRewriter> coroutineRewriter;
    bool alreadyProcessed = false;

    // Track files already processed to avoid double-processing when the
    // compilation database contains duplicate entries for the same file.
    static std::set<std::string> &getProcessedFiles() {
        static std::set<std::string> files;
        return files;
    }

public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        if (!getProcessedFiles().insert(InFile.str()).second) {
            alreadyProcessed = true;
            REWRITE_LOG() << "Skipping already-processed file: " << InFile.str() << "\n";
            return std::make_unique<ASTConsumer>(); // no-op
        }

        // Suppress all compiler warnings — this is a source-to-source
        // transformer, not a linter. We still track errors so that
        // hasErrorOccurred() correctly blocks rewrites on broken input.
        CI.getDiagnostics().setIgnoreAllWarnings(true);

        sourceManager = &CI.getSourceManager();
        rewriter = std::make_unique<Rewriter>();
        rewriter->setSourceMgr(*sourceManager, CI.getLangOpts());

        coroutineRewriter = std::make_unique<CoroutineRewriter>(
            *rewriter, *sourceManager, CI.getDiagnostics(), CI.getLangOpts());

        return std::make_unique<MyASTConsumer>(*coroutineRewriter);
    }

    void EndSourceFileAction() override {
        if (alreadyProcessed) return;

        coroutineRewriter->performRewrites();
        const auto &coroutines = coroutineRewriter->getCoroutines();
        REWRITE_LOG() << "\nSummary: Processed " << coroutines.size() << " coroutine(s)\n";

        SourceLocation mainFileLoc = sourceManager->getLocForStartOfFile(sourceManager->getMainFileID());
        const std::string inputPath = sourceManager->getFilename(mainFileLoc).str();

        const std::string outPath = OutputFile.empty() ? inputPath : OutputFile.getValue();

        if (rewriter->getRewriteBufferFor(sourceManager->getMainFileID())) {
            const RewriteBuffer &RewriteBuf = rewriter->getEditBuffer(sourceManager->getMainFileID());

            std::error_code EC;
            llvm::raw_fd_ostream OS(outPath, EC, llvm::sys::fs::OF_Text);
            if (EC) {
                llvm::errs() << "Error opening file for writing: " << EC.message() << "\n";
                return;
            }

            RewriteBuf.write(OS);
            REWRITE_LOG() << "Wrote output to: " << outPath << "\n";
        } else {
            REWRITE_LOG() << "No changes needed for: " << inputPath << "\n";
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
    verboseLogging() = Verbose;

    if (InPlace && !OutputFile.empty()) {
        llvm::errs() << "error: -i and -o are mutually exclusive\n";
        return 1;
    }
    if (!InPlace && OutputFile.empty()) {
        llvm::errs() << "error: specify -i to rewrite in-place or -o <file> to write to a new file\n";
        return 1;
    }
    if (!OutputFile.empty() && OptionsParser.getSourcePathList().size() > 1) {
        llvm::errs() << "error: -o requires exactly one input file\n";
        return 1;
    }

    // Resolve -o relative to the real CWD now, before ClangTool::run()
    // switches the working directory to the compilation database directory.
    if (!OutputFile.empty()) {
        llvm::SmallString<256> absPath(OutputFile.getValue());
        llvm::sys::fs::make_absolute(absPath);
        OutputFile = std::string(absPath);
    }

    ClangTool Tool(OptionsParser.getCompilations(),
                   OptionsParser.getSourcePathList());

    auto tool = newFrontendActionFactory<CoroutineRewriterFrontendAction>();
    int result = Tool.run(tool.get());
    return result;
}
