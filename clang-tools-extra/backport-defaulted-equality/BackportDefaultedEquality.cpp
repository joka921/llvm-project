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
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
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

static auto getM(const std::string &name) {
    return allOf(unless(isImplicit()), hasName("operator" + name), isDefaulted());
}

static auto getM2(const std::string &op, const std::string &name) {
    return hasMethod(cxxMethodDecl(getM(op)).bind(name));
}

// A matcher that matches a C++ class with a defaulted `operator==`.
static DeclarationMatcher ClassMatcher = cxxRecordDecl(
    anyOf(getM2("==", "method"), getM2("<", "less"), getM2("<=", "less-eq"), getM2(">", "greater"),
          getM2(">=", "greater-eq"), getM2("!=", "not-equal"), getM2("<=>", "spaceship"))).bind("record");

enum Ops {
    LT, LE, EQ, NE, GE, GT
};

// This class first collects all the matching classes, and then via its `rewrite` method rewrites them.
class DefaultOperatorsRewriter : public MatchFinder::MatchCallback {
    // Class for a single matching class
    struct Res {
        // The position of the defaulted operator, needed to later replace it.
        std::vector<std::pair<Ops, SourceRange> > operatorPosition;
        // The name of the class (without enclosing namespaces)
        std::string className;
        // The names of all non-static members of the class.
        std::vector<std::string> fieldNames;
        // The fully-qualified (including base-classes)
        std::vector<std::string> baseClassNames;
        bool hasError = false;
    };

    std::vector<Res> matches;

    Rewriter &rewriter;
    const SourceManager &sourceManager;
    clang::DiagnosticsEngine &diagnosticsEngine;

public :
    DefaultOperatorsRewriter(Rewriter &rewr, const SourceManager &manager, DiagnosticsEngine &engine) : rewriter(rewr),
        sourceManager(manager), diagnosticsEngine(engine) {
    }

    // This method is called for each class that matches the matcher defined above.
    void run(const MatchFinder::MatchResult &Result) override {
        const CXXRecordDecl *Decl = Result.Nodes.getNodeAs<clang::CXXRecordDecl>("record");

        // Don't rewrite classes that are contained in included header files, only rewrite the currently active file.
        auto fileId = sourceManager.getFileID(Decl->getLocation());
        if (fileId != sourceManager.getMainFileID()) {
            return;
        }

        // Append a results entry for the class that has been found.
        matches.emplace_back();
        matches.back().className = Decl->getNameAsString();
        // Store all the base classes.
        for (const CXXBaseSpecifier &base: Decl->bases()) {
            auto name = base.getType()->getAsCXXRecordDecl()->getQualifiedNameAsString();
            matches.back().baseClassNames.push_back(name);
        }
        // Store all the non-static data members
        for (const FieldDecl *Field: Decl->fields()) {
            matches.back().fieldNames.push_back(Field->getNameAsString());
            if (Field->getType()->isArrayType()) {
                unsigned diagID = diagnosticsEngine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                                                    "Can't rewrite c-style array in a defaulted comparison. Please refactor to `std::array`");
                matches.back().hasError = true;
                diagnosticsEngine.Report(sourceManager.getFileLoc(Field->getSourceRange().getBegin()), diagID);
            }
        }

        addIf(Result, "method", EQ);
        addIf(Result, "less", LT);
        addIf(Result, "less-eq", LE);
        addIf(Result, "greater", GT);
        addIf(Result, "greater-eq", GE);
        addIf(Result, "not-equal", NE);

        // TODO<joka921> deduplicate the spaceships.
        addIf(Result, "spaceship", EQ);
        addIf(Result, "spaceship", LT);
        addIf(Result, "spaceship", LE);
        addIf(Result, "spaceship", GT);
        addIf(Result, "spaceship", GE);
        addIf(Result, "spaceship", NE);
        /*
        // Store the position of the defaulted operator.
            if (const CXXMethodDecl *Decl = Result.Nodes.getNodeAs<clang::CXXMethodDecl>("method")) {
                auto name = Decl->getNameAsString();
                outs() << "foudn operator with name '" << name << "'\n";
                matches.back().operatorPosition = Decl->getSourceRange();
            }
            */
    }

    template<typename T>
    void addIf(const T &Result, const std::string &name, Ops op) {
        if (const CXXMethodDecl *Decl = Result.Nodes.template getNodeAs<clang::CXXMethodDecl>(name)) {
            /*
            auto name = Decl->getNameAsString();
            outs() << "foudn operator with name '" << name << "'\n";
            */
            matches.back().operatorPosition.emplace_back(op, Decl->getSourceRange());
        }
    }

    const char *getRep(Ops op) {
        if (op == LT) { return "<"; }
        if (op == LE) { return "<="; }
        if (op == EQ) { return "=="; }
        if (op == NE) { return "!="; }
        if (op == GE) { return ">="; }
        if (op == GT) { return ">"; }
        return "???blödsinn";
    }

    std::string getRewriteFragment(const std::string &left, const std::string &right, Ops op) {
        std::string res;
        res += "    if (!(" + left + " ==  " + right +
                ")) { ";
        if (op == EQ) {
            res += "return false;";
        } else if (op == NE) {
            res += "return true;";
        } else {
            res += "return " + left + " " + getRep(op) + " " + right + ";";
        }
        res += "}\n";
        return res;
    }

    // This function can be manually called once all the matching classes have been processed.
    // It performs the actual rewrite.
    template<typename Match>
    std::string getRewrite(const Match &m, Ops op) {
        const auto &className = m.className;
        // Set up the actual code string for the rewritten operator.
        std::string nameOfOther = m.baseClassNames.empty() && m.fieldNames.empty() ? "" : "otherRhs";
        std::string rewrite = "bool operator" + std::string{getRep(op)} + "(const " + className + "& " + nameOfOther + ") const {\n";
        for (const auto &baseClass: m.baseClassNames) {
            rewrite += getRewriteFragment("static_cast<const " + baseClass + "&>(*this)",
                                          "static_cast<const " + baseClass + "&>(otherRhs)", op);
        }

        for (const auto &mem: m.fieldNames) {
            rewrite += getRewriteFragment(mem, "otherRhs." + mem, op);
        }
        rewrite += (op == EQ || op == LE || op == GE) ? "    return true;\n  }" : "    return false;\n  }";
        return rewrite;
    }

    void rewriteClass(const Res &m) {
        // Set up the actual code string for the rewritten operator.
        std::vector<std::pair<SourceRange, std::string> > rewrites;
        for (const auto &[op, range]: m.operatorPosition) {
            auto it = std::find_if(rewrites.begin(), rewrites.end(),
                                   [&range = range](const auto &el) { return el.first == range; });
            if (it == rewrites.end()) {
                rewrites.emplace_back(range, "");
                it = rewrites.end() - 1;
            }
            it->second += getRewrite(m, op);
        }
        for (const auto &[range, rewrite]: rewrites) {
            rewriter.ReplaceText(range, rewrite);
        }
    }

    // This function can be manually called once all the matching classes have been processed.
    // It performs the actual rewrite.
    void rewrite() {
        for (const auto &m: matches) {
            if (m.hasError) {
                continue;
            }
            rewriteClass(m);
        }
    }
};

class MyASTConsumer : public ASTConsumer {
public:
    using P = std::unique_ptr<Rewriter>;

    MyASTConsumer(DefaultOperatorsRewriter &callback) {
        Finder.addMatcher(ClassMatcher, &callback);
    }

    void HandleTranslationUnit(ASTContext &Context) override {
        // Perform matching
        Finder.matchAST(Context);
    }

private:
    //Rewriter& Rewrite;
    MatchFinder Finder;
};

// This class handles inputs on a per-file base.
class RewriteDefaultOperatorsFrontendAction : public ASTFrontendAction {
    std::unique_ptr<Rewriter> Rewrite = std::make_unique<Rewriter>();
    const SourceManager *SM;
    std::unique_ptr<DefaultOperatorsRewriter> defaultOperatorsRewriter;

public:
    // This function sets up the `DefaultOperatorsRewriter`.
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        // Initialize the Rewriter with the SourceManager from the CompilerInstance
        SM = &CI.getSourceManager();
        defaultOperatorsRewriter = std::make_unique<DefaultOperatorsRewriter>(*Rewrite, *SM, CI.getDiagnostics());
        // Set the rewriter's buffer to the file being processed
        Rewrite->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

        // Set up the callback for replacing text in the source code
        return std::make_unique<MyASTConsumer>(*defaultOperatorsRewriter);
    }

    // TODO<joka921> The following was suggested by ChatGPT to modify the files in place.
    bool BeginSourceFileAction(CompilerInstance &CI) override {
        // Make sure the rewriter is initialized
        return true;
    }

    // Save the modified source code back to the file
    void EndSourceFileAction() override {
        defaultOperatorsRewriter->rewrite();
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
    auto tool = newFrontendActionFactory<RewriteDefaultOperatorsFrontendAction>();
    int result = Tool.run(tool.get());
    return result;
}
