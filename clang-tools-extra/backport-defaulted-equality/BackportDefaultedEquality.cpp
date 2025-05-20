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

// A matcher that matches a C++ class with a defaulted `operator==`.
static DeclarationMatcher ClassMatcher = cxxRecordDecl(
            hasMethod(cxxMethodDecl(
                allOf(unless(isImplicit()), hasName("operator=="), isDefaulted())).bind(
                "method"))
        )
        .
        bind(
            "record"
        );

/*
static DeclarationMatcher GeneratorMatcher = functionDecl(

        hasBody(compoundStmt(
                forEachDescendant(coyieldExpr())
        ))).bind("function");
        */
static DeclarationMatcher GeneratorMatcher = functionDecl(isDefinition(), hasBody(hasDescendant(stmt(coyieldExpr()).bind("coYield"))))
        .bind("function");
/*
static DeclarationMatcher GeneratorMatcher = functionDecl(isDefinition(), hasBody(compoundStmt(hasDescendant(coyieldExpr())))
        ).bind("function");
        */

void collectVarDeclsFromStmt(Stmt* stmt, std::vector<VarDecl*>& outVarDecls) {
    using namespace clang;
    if (!stmt) return;

    class VarDeclCollector : public RecursiveASTVisitor<VarDeclCollector> {
    public:
        std::vector<VarDecl*>& Collected;

        VarDeclCollector(std::vector<VarDecl*>& collected)
            : Collected(collected) {}

        bool TraverseDecl(Decl* D) {
            //outs() << "traversing a decl at " << D << '\n';
            if (! D) { return true;}
            // Skip traversing into class declarations (local classes)
            if (isa<CXXRecordDecl>(D) && cast<CXXRecordDecl>(D)->isLocalClass()) {
                return true;  // Do not recurse into the class
            }

            // Skip lambdas
            if (isa<CXXMethodDecl>(D) &&
                cast<CXXMethodDecl>(D)->getParent()->isLambda()) {
                return true;  // Don't recurse into lambda method
            }

            return RecursiveASTVisitor::TraverseDecl(D);
        }

        bool TraverseLambdaExpr(LambdaExpr* LE) {
            return true; // Don't recurse into the lambda body
        }

        bool VisitDeclStmt(DeclStmt* declStmt) {
            //outs() << "taversing a declStmt at " << declStmt << '\n';
            for (Decl* decl : declStmt->decls()) {
                if (auto* varDecl = dyn_cast<VarDecl>(decl)) {
                    Collected.push_back(varDecl);
                }
            }
            return true;
        }
    };

    VarDeclCollector collector(outVarDecls);
    collector.TraverseStmt(stmt);
}

// This class first collects all the matching generator and then via its `rewrite` method rewrites them.
class GeneratorRewriter : public MatchFinder::MatchCallback {
    // Class for a single matching class
    /*
    struct Res {
        // The position of the defaulted operator, needed to later replace it.
        SourceRange operatorPosition;
        // The name of the class (without enclosing namespaces)
        std::string className;
        // The names of all non-static members of the class.
        std::vector<std::string> fieldNames;
        // The fully-qualified (including base-classes)
        std::vector<std::string> baseClassNames;
        bool hasError = false;
    };

    std::vector<Res> matches;
     */

    Rewriter &rewriter;
    const SourceManager &sourceManager;
    clang::DiagnosticsEngine &diagnosticsEngine;

public :
    GeneratorRewriter(Rewriter &rewr, const SourceManager &manager, DiagnosticsEngine &engine) : rewriter(rewr),
                                                                                                        sourceManager(manager), diagnosticsEngine(engine) {
    }

    struct localVarPositions {
     std::string type;
     std::string name;
     SourceRange pos;
   };

    struct Res {
        SourceRange coroutinePosition;
        std::vector<localVarPositions> decls;
    };
    std::vector<Res> coroutines;

    // This method is called for each class that matches the matcher defined above.
    void run(const MatchFinder::MatchResult &Result) override {
        const auto *Decl = Result.Nodes.getNodeAs<clang::FunctionDecl>("function");
        const auto *stmt = Result.Nodes.getNodeAs<Stmt>("coYield");

        if (!Decl || !stmt) return;
        auto fileId = sourceManager.getFileID(Decl->getLocation());
        if (fileId != sourceManager.getMainFileID()) {
            /*
            outs() << "fileId doesn't match\n";
            unsigned diagID = diagnosticsEngine.getCustomDiagID(clang::DiagnosticsEngine::Remark,
                                                                "found a cppcoro::generator in wrong file.");
            diagnosticsEngine.Report(sourceManager.getFileLoc(Decl->getSourceRange().getBegin()), diagID);
             */
            return;
        }
        //outs() << "found a generator\n";

        ASTContext &Ctx = *Result.Context;

        // Walk up the parent chain of the coroutine stmt
        const Stmt *current = stmt;
        while (current) {
            const auto &parents = Ctx.getParents(*current);
            if (parents.empty()) break;

            const DynTypedNode &parentNode = parents[0];

            if (const auto *parentDecl = parentNode.get<::Decl>()) {
                if (const auto *record = dyn_cast<CXXRecordDecl>(parentDecl)) {
                      outs() << "found local decl while traversing " << record << '\n';
                    if (record->isLambda() || record->isLocalClass()) {
                        return; // Coroutine stmt is nested inside a lambda or local class
                    }
                }
            }

            // Stop when we've reached the function itself
            if (const ::Decl *D = parentNode.get<FunctionDecl>()) {
                if (D == Decl)  {
                    outs() << "break because we've found the parent" << '\n';
                    break;  // OK, we’ve walked all the way to the target function
                    }
            }

            if (const auto *parentStmt = parentNode.get<Stmt>()) {
                current = parentStmt;
            } else {
                outs() << "break because we've found nothing..." << '\n';
                return;
            }
        }


        std::vector<VarDecl*> decls;
        coroutines.emplace_back();
        auto& c = coroutines.back();
        coroutines.back().coroutinePosition = Decl->getSourceRange();

        auto body = Decl->getBody();
        if (! body) {
            return;
        }
        //outs() << "collecting decls, body is " << body <<"\n" ;
        collectVarDeclsFromStmt(Decl->getBody(), decls);
        //outs() << "finished collecting decls\n";
        for (auto* varDecl : decls) {
            c.decls.push_back(localVarPositions{varDecl->getNameAsString(), varDecl->getType().getAsString(), varDecl->getSourceRange()});
            auto&b = c.decls.back();
            outs() << "found a member variable \"" << b.name << "\" with type \"" << b.type << "\"\n";
        }
        //outs() << "after printing decls\n";

        // Don't rewrite classes that are contained in included header files, only rewrite the currently active file.
        unsigned diagID = diagnosticsEngine.getCustomDiagID(clang::DiagnosticsEngine::Remark,
                                                            "found a cppcoro::generator.");
        diagnosticsEngine.Report(sourceManager.getFileLoc(Decl->getSourceRange().getBegin()), diagID);
    }

    // This function can be manually called once all the matching classes have been processed.
    // It performs the actual rewrite.
    void rewrite() {
    return;
    }
};

// This class first collects all the matching classes, and then via its `rewrite` method rewrites them.
class DefaultOperatorsRewriter : public MatchFinder::MatchCallback {
    // Class for a single matching class
    struct Res {
        // The position of the defaulted operator, needed to later replace it.
        SourceRange operatorPosition;
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
    // Store the position of the defaulted operator.
        if (const CXXMethodDecl *Decl = Result.Nodes.getNodeAs<clang::CXXMethodDecl>("method")) {
            auto name = Decl->getNameAsString();
            outs() << "foudn operator with name '" << name << "'\n";
            matches.back().operatorPosition = Decl->getSourceRange();
        }
    }

    // This function can be manually called once all the matching classes have been processed.
    // It performs the actual rewrite.
    void rewrite() {
        for (const auto &m: matches) {
            if (m.hasError) {
                continue;
            }
            const auto &className = m.className;
            // Set up the actual code string for the rewritten operator.
            std::string nameOfOther = m.baseClassNames.empty() && m.fieldNames.empty() ? "" : "otherRhs";
            std::string rewrite = "bool operator==(const " + className + "& " + nameOfOther + ") const {\n";
            for (const auto &baseClass: m.baseClassNames) {
                rewrite += "    if (!(static_cast<const " + baseClass + "&>(*this) == static_cast<const " + baseClass +
                        "&>(otherRhs))) { return false;}\n";
            }

            for (const auto &mem: m.fieldNames) {
                rewrite += "    if (!(" + mem + " == otherRhs." + mem + ")) {return false;}\n";
            }
            rewrite += "    return true;\n  }";
            rewriter.ReplaceText(m.operatorPosition, rewrite);
        }
    }
};

class MyASTConsumer : public ASTConsumer {
public:
    using P = std::unique_ptr<Rewriter>;

    MyASTConsumer(DefaultOperatorsRewriter &callback, GeneratorRewriter& generatorCallback) {
        Finder.addMatcher(ClassMatcher, &callback);
        Finder.addMatcher(GeneratorMatcher, &generatorCallback);
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
    std::unique_ptr<GeneratorRewriter> generatorRewriter;

public:
    // This function sets up the `DefaultOperatorsRewriter`.
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance &CI, llvm::StringRef InFile) override {
        // Initialize the Rewriter with the SourceManager from the CompilerInstance
        SM = &CI.getSourceManager();
        defaultOperatorsRewriter = std::make_unique<DefaultOperatorsRewriter>(*Rewrite, *SM, CI.getDiagnostics());
        generatorRewriter = std::make_unique<GeneratorRewriter>(*Rewrite, *SM, CI.getDiagnostics());

        // Set the rewriter's buffer to the file being processed
        Rewrite->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());

        // Set up the callback for replacing text in the source code
        return std::make_unique<MyASTConsumer>(*defaultOperatorsRewriter, *generatorRewriter);
    }

    // TODO<joka921> The following was suggested by ChatGPT to modify the files in place.
    bool BeginSourceFileAction(CompilerInstance &CI) override {
        // Make sure the rewriter is initialized
        return true;
    }

    // Save the modified source code back to the file
    void EndSourceFileAction() override {
        // TODO<joka921> This early return has to be removed to fix the final
        // thing.
        return;
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
