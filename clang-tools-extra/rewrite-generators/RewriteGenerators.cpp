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
#include <limits>
#include <map>

#include "./LocalVariableCollector.h"
using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

// Logging macro for fine-grained control
#define REWRITE_LOG() std::cout


// Helper function to generate state get() call
static std::string makeStateGetCall(const std::string &varName) {
    return "CO_GET(" + varName + ")";
    //return "this->state." + varName + ".get()";
}

// Helper function to generate state construct() call
static std::string makeStateConstructCall(const std::string &memberName, const std::string &initializer) {
    return "this->state." + memberName + ".construct(" + initializer + ")";
}

// Helper function to generate state destroy() call
static std::string makeStateDestroyCall(const std::string &memberName) {
    return "this->state." + memberName + ".destroy()";
}

// Helper function to generate state construct prefix (for declaration replacement)
static std::string makeStateConstructPrefix(const std::string &memberName) {
    return "this->state." + memberName + ".construct(";
}

static llvm::cl::OptionCategory MyToolCategory("coroutine-rewriter");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("\nRewrites C++20 coroutines to C++17 compatible state machines.\n");


struct RangedForLoop {
    const CXXForRangeStmt *stmt;
    std::string loopVarName;
    std::string loopVarType;
    Expr* rangeExpr;
    std::string rangeVarName; // e.g., "__range_0"
    std::string beginVarName; // e.g., "__begin_0"
    std::string endVarName; // e.g., "__end_0"
    unsigned index;
    SourceRange fullRange;
};

struct FunctionParameter {
    std::string name;
    std::string type;
    QualType qualType;
};

struct CoroutineInfo {
    const FunctionDecl *function;
    std::set<LocalVariable> localVariables;
    std::vector<RangedForLoop> rangedForLoops; // Add ranged-for loop info
    std::vector<FunctionParameter> parameters; // Function parameters
    SourceLocation insertionPoint;
    bool hasError = false;
};


struct ScopeInfo {
    const CompoundStmt *compoundStmt;
    std::vector<std::string> variablesInScope;
    SourceLocation scopeEnd;
};

struct ScopeEndReplacement {
    std::string replacement;
    int priority;
    bool insertAfterBrace = false;

    bool operator<(const ScopeEndReplacement &other) const {
        return insertAfterBrace != other.insertAfterBrace ? other.insertAfterBrace : priority < other.priority;
    }
};

struct CoroutineStatement {
    enum Type { YIELD, AWAIT };

    Type type;
    const Stmt *stmt; // CoawaitExpr* or CoyieldExpr*
    const Expr *operand; // The expression being yielded/awaited
    unsigned index;
    SourceLocation keywordLoc;
    SourceLocation operandStart;
    SourceLocation operandEnd;
};


class CoroutineBodyRewriter : public RecursiveASTVisitor<CoroutineBodyRewriter> {
private:
    const std::set<LocalVariable> &localVariables;
    Rewriter &rewriter;
    const SourceManager &sourceManager;
    std::set<std::string> variableNames;
    std::vector<std::pair<SourceRange, std::string> > declReplacements;
    std::vector<std::pair<SourceRange, std::string> > refReplacements;
    std::set<SourceLocation> processedDeclarations;
    std::vector<ScopeInfo> scopeStack;
    std::vector<std::pair<SourceLocation, std::string> > destructorInsertions;
    std::vector<CoroutineStatement> coroutineStatements;
    unsigned nextCoroStatementIndex;
    std::vector<RangedForLoop> rangedForLoops;
    unsigned nextRangedForIndex;

    // Map from closing brace position to all replacements that should happen at that position

    // Global replacement vector for ALL types of replacements (SourceRange, replacement_string, priority)
public:
    std::map<unsigned, std::vector<ScopeEndReplacement> > scopeEndReplacements;
    std::vector<std::tuple<SourceRange, std::string, int> > globalReplacements;

public:
    CoroutineBodyRewriter(const std::set<LocalVariable> &vars, Rewriter &rewr, const SourceManager &SM)
        : localVariables(vars), rewriter(rewr), sourceManager(SM), nextCoroStatementIndex(1), nextRangedForIndex(0) {
        // Create a set of variable names for quick lookup
        for (const auto &var: localVariables) {
            variableNames.insert(var.name);
        }
    }

    // Track compound statements (scopes)
    bool VisitCompoundStmt(CompoundStmt *compoundStmt) {
        REWRITE_LOG() << "  DEBUG: Entering scope (CompoundStmt)\n";

        // Create scope info
        ScopeInfo scope;
        scope.compoundStmt = compoundStmt;
        scope.scopeEnd = compoundStmt->getRBracLoc();

        // Push scope onto stack
        scopeStack.push_back(scope);

        // Traverse children manually to have control over when we pop the scope
        for (auto *child: compoundStmt->children()) {
            if (child) {
                TraverseStmt(child);
            }
        }

        // Before popping scope, insert destructor calls
        if (!scopeStack.empty() && !scopeStack.back().variablesInScope.empty()) {
            insertDestructorsForScope(scopeStack.back());
        }

        // Pop scope
        if (!scopeStack.empty()) {
            scopeStack.pop_back();
        }

        REWRITE_LOG() << "  DEBUG: Exiting scope (CompoundStmt)\n";

        return false; // We handled traversal manually
    }

    // Handle variable declarations - collect for later processing
    bool VisitDeclStmt(DeclStmt *declStmt) {
        for (auto *decl: declStmt->decls()) {
            if (auto *varDecl = dyn_cast<VarDecl>(decl)) {
                std::string varName = varDecl->getNameAsString();

                if (variableNames.count(varName)) {
                    REWRITE_LOG() << "  Found variable declaration: " << varName << "\n";

                    SourceLocation declLoc = varDecl->getLocation();
                    if (processedDeclarations.count(declLoc)) {
                        continue; // Already processed this declaration
                    }
                    processedDeclarations.insert(declLoc);

                    // Add variable to current scope
                    if (!scopeStack.empty()) {
                        scopeStack.back().variablesInScope.push_back(varName);
                        REWRITE_LOG() << "    DEBUG: Added variable '" << varName << "' to current scope\n";
                    }

                    // Create construct prefix and add replacement for declaration part
                    std::string constructPrefix = makeStateConstructPrefix(varName);
                    
                    if (varDecl->hasInit()) {
                        // Get the range for just the declaration part (type + name)
                        SourceLocation declStart = varDecl->getSourceRange().getBegin();
                        SourceLocation initStart = varDecl->getInit()->getSourceRange().getBegin();
                        SourceRange declOnlyRange(declStart, initStart.getLocWithOffset(-1));
                        
                        // Replace declaration part with construct prefix
                        declReplacements.emplace_back(declOnlyRange, constructPrefix);
                        
                        // Recursively visit the initialization expression
                        TraverseStmt(varDecl->getInit());
                        
                        // Add closing parenthesis and semicolon at the end
                        SourceLocation initEnd = varDecl->getInit()->getSourceRange().getEnd();
                        SourceLocation afterInit = Lexer::getLocForEndOfToken(initEnd, 0, sourceManager, LangOptions());
                        SourceRange closingRange(afterInit, afterInit);
                        declReplacements.emplace_back(closingRange, ");");
                    } else {
                        // No initialization - replace entire declaration with empty construct call
                        std::string constructCall = constructPrefix + ");";
                        SourceRange declRange = varDecl->getSourceRange();
                        declReplacements.emplace_back(declRange, constructCall);
                    }
                }
            }
        }
        return true; // Continue traversing
    }

    // Handle co_yield expressions
    bool VisitCoyieldExpr(CoyieldExpr *coyield) {
        REWRITE_LOG() << "  Found co_yield expression\n";

        CoroutineStatement coroStmt;
        coroStmt.type = CoroutineStatement::YIELD;
        coroStmt.stmt = coyield;
        coroStmt.operand = coyield->getOperand();
        coroStmt.index = nextCoroStatementIndex++;

        // Find the location of the co_yield keyword
        coroStmt.keywordLoc = coyield->getKeywordLoc();

        // Get the operand range
        if (coroStmt.operand) {
            coroStmt.operandStart = coroStmt.operand->getBeginLoc();
            coroStmt.operandEnd = coroStmt.operand->getEndLoc();
        }

        coroutineStatements.push_back(coroStmt);

        REWRITE_LOG() << "    DEBUG: Added co_yield with index " << coroStmt.index << "\n";

        return true;
    }

    // Handle co_await expressions
    bool VisitCoawaitExpr(CoawaitExpr *coawait) {
        REWRITE_LOG() << "  Found co_await expression\n";

        CoroutineStatement coroStmt;
        coroStmt.type = CoroutineStatement::AWAIT;
        coroStmt.stmt = coawait;
        coroStmt.operand = coawait->getOperand();
        coroStmt.index = nextCoroStatementIndex++;

        // Find the location of the co_await keyword
        coroStmt.keywordLoc = coawait->getKeywordLoc();

        // Get the operand range
        if (coroStmt.operand) {
            coroStmt.operandStart = coroStmt.operand->getBeginLoc();
            coroStmt.operandEnd = coroStmt.operand->getEndLoc();
        }

        coroutineStatements.push_back(coroStmt);

        REWRITE_LOG() << "    DEBUG: Added co_await with index " << coroStmt.index << "\n";

        return true;
    }

    // Handle ranged-for loops
    bool VisitCXXForRangeStmt(CXXForRangeStmt *forRange) {
        REWRITE_LOG() << "\n=== RANGED-FOR LOOP DETECTION ===\n";
        REWRITE_LOG() << "  Found ranged-for loop at: " << forRange->getSourceRange().getBegin().printToString(
                    sourceManager)
                << " to " << forRange->getSourceRange().getEnd().printToString(sourceManager) << "\n";

        RangedForLoop rangedFor;
        rangedFor.stmt = forRange;
        rangedFor.index = nextRangedForIndex++;
        rangedFor.fullRange = forRange->getSourceRange();

        // Generate unique variable names for this loop
        rangedFor.rangeVarName = "__range_" + std::to_string(rangedFor.index);
        rangedFor.beginVarName = "__begin_" + std::to_string(rangedFor.index);
        rangedFor.endVarName = "__end_" + std::to_string(rangedFor.index);

        // Extract loop variable information
        const VarDecl *loopVar = forRange->getLoopVariable();
        if (loopVar) {
            rangedFor.loopVarName = loopVar->getNameAsString();
            QualType loopVarType = loopVar->getType();
            rangedFor.loopVarType = loopVarType.getAsString();
            REWRITE_LOG() << "    Loop variable: " << rangedFor.loopVarType << " " << rangedFor.loopVarName
                    << " at " << loopVar->getLocation().printToString(sourceManager) << "\n";
        }

        // Extract range expression
        const Expr *rangeExpr = forRange->getRangeInit();
        if (rangeExpr) {
            rangedFor.rangeExpr = forRange->getRangeInit();
            /*
            SourceRange rangeRange = rangeExpr->getSourceRange();
            CharSourceRange charRange = CharSourceRange::getTokenRange(rangeRange);
            //rangedFor.rangeExpr = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
            REWRITE_LOG() << "    Range expression: '" << rangedFor.rangeExpr << "' at "
                    << rangeRange.getBegin().printToString(sourceManager) << "\n";
                    */
        }

        // Extract detailed position information
        REWRITE_LOG() << "  DETAILED POSITIONS:\n";
        REWRITE_LOG() << "    For keyword: " << forRange->getForLoc().printToString(sourceManager) << "\n";
        REWRITE_LOG() << "    Colon location: " << forRange->getColonLoc().printToString(sourceManager) << "\n";
        REWRITE_LOG() << "    RParenLoc: " << forRange->getRParenLoc().printToString(sourceManager) << "\n";

        const Stmt *body = forRange->getBody();
        if (body) {
            REWRITE_LOG() << "    Body type: " << body->getStmtClassName() << "\n";
            REWRITE_LOG() << "    Body range: " << body->getSourceRange().getBegin().printToString(sourceManager)
                    << " to " << body->getSourceRange().getEnd().printToString(sourceManager) << "\n";

            if (auto *compoundBody = dyn_cast<CompoundStmt>(body)) {
                REWRITE_LOG() << "    Body LBraceLoc: " << compoundBody->getLBracLoc().printToString(sourceManager) <<
                        "\n";
                REWRITE_LOG() << "    Body RBraceLoc: " << compoundBody->getRBracLoc().printToString(sourceManager) <<
                        "\n";
                REWRITE_LOG() << "    Body has " << compoundBody->size() << " child statements\n";

                int childIndex = 0;
                for (const auto *child: compoundBody->children()) {
                    if (child) {
                        REWRITE_LOG() << "      Child[" << childIndex << "]: " << child->getStmtClassName()
                                << " at " << child->getSourceRange().getBegin().printToString(sourceManager)
                                << " to " << child->getSourceRange().getEnd().printToString(sourceManager) << "\n";
                    }
                    childIndex++;
                }
            }
        }

        REWRITE_LOG() << "    Generated variable names: " << rangedFor.rangeVarName
                << ", " << rangedFor.beginVarName << ", " << rangedFor.endVarName << "\n";

        rangedForLoops.push_back(rangedFor);

        // CRITICAL: Must manually traverse the loop body since we're overriding VisitCXXForRangeStmt
        REWRITE_LOG() << "    MANUALLY traversing loop body for normal coroutine rewriting...\n";
        if (body) {
            REWRITE_LOG() << "      Starting manual traversal of body: " << body->getStmtClassName() << "\n";
            TraverseStmt(const_cast<Stmt *>(body));
            REWRITE_LOG() << "      Completed manual traversal of body\n";
        } else {
            REWRITE_LOG() << "      WARNING: No body to traverse\n";
        }

        // IMPORTANT: Return false to prevent automatic traversal (we did it manually)
        REWRITE_LOG() << "    Returning false to prevent double traversal\n";
        REWRITE_LOG() << "=== END RANGED-FOR DETECTION ===\n\n";

        return false;
    }

    // Handle variable references - but not in declaration contexts
    bool VisitDeclRefExpr(DeclRefExpr *declRef) {
        if (auto *varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
            std::string varName = varDecl->getNameAsString();

            if (variableNames.count(varName)) {
                // Check if this reference is part of a declaration we're already handling
                if (!isPartOfDeclaration(declRef)) {
                    REWRITE_LOG() << "  Found variable reference: " << varName << "\n";

                    std::string getCall = makeStateGetCall(varName);
                    SourceRange refRange = declRef->getSourceRange();
                    refReplacements.emplace_back(refRange, getCall);
                }
            }
        }
        return true;
    }

private:
    std::string getInitializationArguments(const VarDecl *varDecl) {
        std::string varName = varDecl->getNameAsString();
        REWRITE_LOG() << "    DEBUG: Processing initialization for variable: " << varName << "\n";

        if (!varDecl->hasInit()) {
            REWRITE_LOG() << "    DEBUG: No initialization for " << varName << "\n";
            return "";
        }

        const Expr *init = varDecl->getInit();
        REWRITE_LOG() << "    DEBUG: Initialization expression type: " << init->getStmtClassName() << "\n";

        // Get raw text for debugging
        SourceRange range = init->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(range);
        std::string rawText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
        REWRITE_LOG() << "    DEBUG: Raw initialization text: '" << rawText << "'\n";

        // Check if this is direct list initialization (T var{args}) vs copy initialization (T var = {args})
        bool isDirectListInit = rawText.find(varName + "{") == 0;
        REWRITE_LOG() << "    DEBUG: Is direct list initialization (T var{...}): " << (
            isDirectListInit ? "true" : "false") << "\n";

        std::string result;

        if (isDirectListInit) {
            // For T var{args}, extract just the {args} part
            REWRITE_LOG() << "    DEBUG: Handling direct list initialization\n";
            size_t bracePos = rawText.find('{');
            if (bracePos != std::string::npos) {
                std::string listPart = rawText.substr(bracePos);
                REWRITE_LOG() << "    DEBUG: Extracted list part: '" << listPart << "'\n";

                // Still need to rewrite any variable references within the list
                result = rewriteVariableReferencesInText(listPart, varName);
            } else {
                REWRITE_LOG() << "    DEBUG: No brace found in direct list init, fallback to normal processing\n";
                result = processInitExpression(init, varName);
            }
        } else {
            // Handle other initialization styles normally
            result = processInitExpression(init, varName);
        }

        REWRITE_LOG() << "    DEBUG: Final initialization arguments: '" << result << "'\n";
        return result;
    }

    std::string processInitExpression(const Expr *init, const std::string &currentVarName) {
        // Handle different initialization styles
        if (auto *constructExpr = dyn_cast<CXXConstructExpr>(init)) {
            REWRITE_LOG() << "    DEBUG: Found CXXConstructExpr\n";
            return handleConstructorArgs(constructExpr);
        } else if (auto *initListExpr = dyn_cast<InitListExpr>(init)) {
            REWRITE_LOG() << "    DEBUG: Found InitListExpr\n";
            return handleInitListArgs(initListExpr);
        } else {
            REWRITE_LOG() << "    DEBUG: Using rewriteExpression for other type\n";
            return rewriteExpressionExceptVar(init, currentVarName);
        }
    }

    std::string rewriteVariableReferencesInText(const std::string &text, const std::string &excludeVar) {
        REWRITE_LOG() << "        DEBUG: rewriteVariableReferencesInText input: '" << text << "'\n";
        REWRITE_LOG() << "        DEBUG: Excluding variable: '" << excludeVar << "'\n";

        std::string result = text;

        // Replace variable references in the text, but exclude the current variable being declared
        for (const auto &varName: variableNames) {
            if (varName == excludeVar) {
                REWRITE_LOG() << "        DEBUG: Skipping replacement of '" << varName << "' (current variable)\n";
                continue; // Don't replace the variable we're currently declaring
            }

            std::string replacement = makeStateGetCall(varName);

            size_t pos = 0;
            while ((pos = result.find(varName, pos)) != std::string::npos) {
                // Check if it's a word boundary
                bool isWordStart = (pos == 0) || !std::isalnum(result[pos - 1]);
                bool isWordEnd = (pos + varName.length() >= result.length()) ||
                                 !std::isalnum(result[pos + varName.length()]);

                if (isWordStart && isWordEnd) {
                    REWRITE_LOG() << "        DEBUG: Replacing '" << varName << "' with '" << replacement <<
                            "' at position " << pos << "\n";
                    result.replace(pos, varName.length(), replacement);
                    pos += replacement.length();
                } else {
                    pos += varName.length();
                }
            }
        }

        REWRITE_LOG() << "        DEBUG: rewriteVariableReferencesInText output: '" << result << "'\n";
        return result;
    }

    std::string handleConstructorArgs(const CXXConstructExpr *constructExpr) {
        REWRITE_LOG() << "      DEBUG: handleConstructorArgs - found " << constructExpr->getNumArgs() << " arguments\n";
        std::string args;

        for (unsigned i = 0; i < constructExpr->getNumArgs(); ++i) {
            if (i > 0) args += ", ";
            const Expr *arg = constructExpr->getArg(i);
            std::string argText = rewriteExpression(arg);
            REWRITE_LOG() << "      DEBUG: Constructor arg " << i << ": '" << argText << "'\n";
            args += argText;
        }

        REWRITE_LOG() << "      DEBUG: handleConstructorArgs result: '" << args << "'\n";
        return args;
    }

    std::string handleInitListArgs(const InitListExpr *initList) {
        REWRITE_LOG() << "      DEBUG: handleInitListArgs - found " << initList->getNumInits() << " elements\n";
        std::string args = "{";

        for (unsigned i = 0; i < initList->getNumInits(); ++i) {
            if (i > 0) args += ", ";
            const Expr *init = initList->getInit(i);
            std::string initText = rewriteExpression(init);
            REWRITE_LOG() << "      DEBUG: InitList element " << i << ": '" << initText << "'\n";
            args += initText;
        }

        args += "}";
        REWRITE_LOG() << "      DEBUG: handleInitListArgs result: '" << args << "'\n";
        return args;
    }

    std::string rewriteExpression(const Expr *expr) {
        if (!expr) return "";

        SourceRange range = expr->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(range);
        std::string exprText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();

        REWRITE_LOG() << "        DEBUG: rewriteExpression input: '" << exprText << "'\n";
        REWRITE_LOG() << "        DEBUG: Expression type: " << expr->getStmtClassName() << "\n";

        // Replace variable references in the expression
        std::string originalText = exprText;
        for (const auto &varName: variableNames) {
            std::string replacement = makeStateGetCall(varName);

            size_t pos = 0;
            while ((pos = exprText.find(varName, pos)) != std::string::npos) {
                // Check if it's a word boundary
                bool isWordStart = (pos == 0) || !std::isalnum(exprText[pos - 1]);
                bool isWordEnd = (pos + varName.length() >= exprText.length()) ||
                                 !std::isalnum(exprText[pos + varName.length()]);

                if (isWordStart && isWordEnd) {
                    REWRITE_LOG() << "        DEBUG: Replacing '" << varName << "' with '" << replacement <<
                            "' at position " << pos << "\n";
                    exprText.replace(pos, varName.length(), replacement);
                    pos += replacement.length();
                } else {
                    pos += varName.length();
                }
            }
        }

        if (originalText != exprText) {
            REWRITE_LOG() << "        DEBUG: rewriteExpression output: '" << exprText << "'\n";
        } else {
            REWRITE_LOG() << "        DEBUG: No changes made to expression\n";
        }

        return exprText;
    }

    std::string rewriteExpressionExceptVar(const Expr *expr, const std::string &excludeVar) {
        if (!expr) return "";

        SourceRange range = expr->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(range);
        std::string exprText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();

        REWRITE_LOG() << "        DEBUG: rewriteExpressionExceptVar input: '" << exprText << "'\n";
        REWRITE_LOG() << "        DEBUG: Expression type: " << expr->getStmtClassName() << "\n";
        REWRITE_LOG() << "        DEBUG: Excluding variable: '" << excludeVar << "'\n";

        return rewriteVariableReferencesInText(exprText, excludeVar);
    }

    void insertDestructorsForScope(const ScopeInfo &scope) {
        REWRITE_LOG() << "    DEBUG: Inserting destructors for scope with " << scope.variablesInScope.size() <<
                " variables\n";

        if (scope.variablesInScope.empty()) {
            return;
        }

        // Find variables in this scope and sort by their priority for proper destruction order
        std::vector<std::pair<std::string, int> > scopeVarsWithPriority;
        for (const std::string &varName: scope.variablesInScope) {
            // Find the variable's priority
            for (const auto &var: localVariables) {
                if (var.name == varName) {
                    scopeVarsWithPriority.emplace_back(varName, var.priority);
                    break;
                }
            }
        }

        // Sort by priority in reverse order for destruction (highest priority destroyed last)
        std::sort(scopeVarsWithPriority.begin(), scopeVarsWithPriority.end(),
                  [](const auto &a, const auto &b) { return a.second > b.second; });

        // Add destructor calls to the scope end replacements map
        SourceLocation insertLoc = scope.scopeEnd;
        unsigned fileOffset = sourceManager.getFileOffset(insertLoc);

        for (size_t i = 0; i < scopeVarsWithPriority.size(); ++i) {
            const std::string &varName = scopeVarsWithPriority[i].first;
            int varPriority = scopeVarsWithPriority[i].second;

            std::string destructorCall = "    " + makeStateDestroyCall(varName) + ";\n";

            // Use negative priority to ensure reverse order at same location
            int destructorPriority = -varPriority - static_cast<int>(i);

            ScopeEndReplacement replacement;
            replacement.replacement = destructorCall;
            replacement.priority = destructorPriority;

            scopeEndReplacements[fileOffset].push_back(replacement);

            REWRITE_LOG() << "      DEBUG: Added destructor call for variable: " << varName
                    << " with priority: " << destructorPriority << " at file offset " << fileOffset << "\n";
        }
    }

    bool isPartOfDeclaration(const DeclRefExpr *declRef) {
        // Check if this reference is part of a declaration statement we're processing
        const Stmt *parent = declRef;
        while (parent) {
            if (auto *declStmt = dyn_cast<DeclStmt>(parent)) {
                for (auto *decl: declStmt->decls()) {
                    if (auto *varDecl = dyn_cast<VarDecl>(decl)) {
                        if (variableNames.count(varDecl->getNameAsString())) {
                            return true;
                        }
                    }
                }
            }
            // This is a simplified check - in a full implementation we'd traverse the parent chain
            break;
        }
        return false;
    }

    void collectCoroutineStatementReplacements() {
        REWRITE_LOG() << "  DEBUG: Collecting coroutine statement replacements for " << coroutineStatements.size() <<
                " statements\n";

        for (const auto &coroStmt: coroutineStatements) {
            if (coroStmt.type == CoroutineStatement::YIELD) {
                REWRITE_LOG() << "    DEBUG: Collecting co_yield replacement for CO_YIELD(" << coroStmt.index <<
                        ", ...)\n";

                // Replace "co_yield" with "CO_YIELD(index, "
                std::string macroStart = "CO_YIELD(" + std::to_string(coroStmt.index) + ", ";

                // Priority based on index for consistent ordering
                int priority = static_cast<int>(coroStmt.index);

                // Create SourceRange from keyword location
                SourceRange keywordRange(coroStmt.keywordLoc, coroStmt.keywordLoc);
                globalReplacements.emplace_back(keywordRange, macroStart, priority);

                // Add closing parenthesis after the operand
                if (coroStmt.operand) {
                    SourceLocation operandEnd = Lexer::getLocForEndOfToken(
                        coroStmt.operandEnd, 0, sourceManager, LangOptions());
                    SourceRange parenRange(operandEnd, operandEnd);
                    globalReplacements.emplace_back(parenRange, ")", priority + 1000); // Later priority
                }
            } else if (coroStmt.type == CoroutineStatement::AWAIT) {
                REWRITE_LOG() << "    DEBUG: Collecting co_await replacement for CO_AWAIT(" << coroStmt.index <<
                        ", ...)\n";

                // Replace "co_await" with "CO_AWAIT(index, "
                std::string macroStart = "CO_AWAIT(" + std::to_string(coroStmt.index) + ", ";

                // Priority based on index for consistent ordering
                int priority = static_cast<int>(coroStmt.index);

                // Create SourceRange from keyword location
                SourceRange keywordRange(coroStmt.keywordLoc, coroStmt.keywordLoc);
                globalReplacements.emplace_back(keywordRange, macroStart, priority);

                // Add closing parenthesis after the operand
                if (coroStmt.operand) {
                    SourceLocation operandEnd = Lexer::getLocForEndOfToken(
                        coroStmt.operandEnd, 0, sourceManager, LangOptions());
                    SourceRange parenRange(operandEnd, operandEnd);
                    globalReplacements.emplace_back(parenRange, ")", priority + 1000); // Later priority
                }
            }
        }

        REWRITE_LOG() << "  Collected " << coroutineStatements.size() <<
                " coroutine statement replacements into global vector\n";
    }

private:
    void collectRangedForFooterInsertions() {
        REWRITE_LOG() <<
                "  DEBUG: Collecting ranged-for footer insertions (destroy calls) into scope end replacements\n";

        for (const auto &rangedFor: rangedForLoops) {
            // Find the location where footer destroy calls should be inserted
            // This should be AFTER the entire for loop statement (after its closing brace)
            const Stmt *body = rangedFor.stmt->getBody();
            if (body) {
                if (auto *compoundBody = dyn_cast<CompoundStmt>(body)) {
                    // Calculate where the footer should go in the transformed code
                    // In the explicit form, the footer goes after "  }" (the for loop close) but before "}" (scope close)
                    SourceLocation footerLoc = compoundBody->getRBracLoc();
                    unsigned fileOffset = sourceManager.getFileOffset(footerLoc);

                    // Create destroy calls for __end, __begin, __range (reverse order of construction)
                    std::vector<std::string> destroyCalls = {
                        "    " + makeStateDestroyCall(rangedFor.endVarName) + ";\n",
                        "    " + makeStateDestroyCall(rangedFor.beginVarName) + ";\n",
                        "    " + makeStateDestroyCall(rangedFor.rangeVarName) + ";\n"
                    };

                    // Add each destroy call with high priorities to ensure they come after loop variable destruction
                    int basePriority = 50000 + static_cast<int>(rangedFor.index) * 10;

                    for (size_t i = 0; i < destroyCalls.size(); ++i) {
                        ScopeEndReplacement replacement;
                        replacement.replacement = destroyCalls[i];
                        replacement.priority = basePriority + static_cast<int>(i);
                        replacement.insertAfterBrace = true;

                        scopeEndReplacements[fileOffset].push_back(replacement);

                        REWRITE_LOG() << "    Added footer destroy call: " << destroyCalls[i].substr(0, 40) << "... "
                                << "at " << footerLoc.printToString(sourceManager)
                                << " (priority " << replacement.priority << ", file offset " << fileOffset << ")\n";
                    }
                }
            }
        }

        REWRITE_LOG() << "  Collected footer destroy calls for " << rangedForLoops.size() << " ranged-for loops\n";
    }

public:
    void collectRangedForLoopReplacements() {
        REWRITE_LOG() << "  DEBUG: Collecting ranged-for loop replacements for " << rangedForLoops.size() << " loops\n";

        // First collect footer insertions for all loops
        collectRangedForFooterInsertions();

        for (const auto &rangedFor: rangedForLoops) {
            REWRITE_LOG() << "\n=== COLLECTING RANGED-FOR REPLACEMENTS ===\n";
            REWRITE_LOG() << "    DEBUG: Processing ranged-for loop " << rangedFor.index << "\n";

            // Part 1: Replace the for statement header only
            collectRangedForHeaderReplacement(rangedFor);

            // Part 2: Insert loop variable construct after opening brace
            collectRangedForLoopVarConstruct(rangedFor);

            // Part 3: Insert loop variable destroy before closing brace
            collectRangedForLoopVarDestroy(rangedFor);

            REWRITE_LOG() << "=== END COLLECTING RANGED-FOR REPLACEMENTS ===\n\n";
        }

        REWRITE_LOG() << "  Collected " << rangedForLoops.size() << " ranged-for loop replacements\n";
    }

private:
    void collectRangedForHeaderReplacement(const RangedForLoop &rangedFor) {
        REWRITE_LOG() << "      DEBUG: Collecting header replacement for ranged-for loop " << rangedFor.index << "\n";

        // Find the range from "for" keyword to the closing parenthesis
        const CXXForRangeStmt *forStmt = rangedFor.stmt;
        SourceLocation forLoc = forStmt->getForLoc();
        SourceLocation rParenLoc = forStmt->getRParenLoc();

        if (forLoc.isValid() && rParenLoc.isValid()) {
            SourceRange headerRange(forLoc, rParenLoc);

            // Generate the construct calls for __range, __begin, __end followed by the for loop
            std::string constructCalls =
                    makeStateConstructCall(rangedFor.rangeVarName, getSourceText(rangedFor.rangeExpr, sourceManager)) + ";\n" +
                    "    " + makeStateConstructCall(rangedFor.beginVarName, makeStateGetCall(
                        rangedFor.rangeVarName) + ".begin()") + ";\n" +
                    "    " + makeStateConstructCall(rangedFor.endVarName, makeStateGetCall(
                        rangedFor.rangeVarName) + ".end()") + ";\n" +
                    "    for (; " + makeStateGetCall(rangedFor.beginVarName) + " != " + makeStateGetCall(
                        rangedFor.endVarName) +
                    "; ++" + makeStateGetCall(rangedFor.beginVarName) + ")";

            int headerPriority = 10000 + static_cast<int>(rangedFor.index);
            globalReplacements.emplace_back(headerRange, constructCalls, headerPriority);

            REWRITE_LOG() << "        Added header replacement with construct calls at "
                    << headerRange.printToString(sourceManager) << " (priority " << headerPriority << ")\n";
            REWRITE_LOG() << "        Construct calls: " << constructCalls.substr(0, 100) << "...\n";
        } else {
            REWRITE_LOG() << "        ERROR: Invalid for statement locations\n";
        }
    }

    void collectRangedForLoopVarConstruct(const RangedForLoop &rangedFor) {
        REWRITE_LOG() << "      DEBUG: Collecting loop var construct for ranged-for loop " << rangedFor.index << "\n";

        // Find the opening brace of the loop body
        const Stmt *body = rangedFor.stmt->getBody();
        if (auto *compoundBody = dyn_cast<CompoundStmt>(body)) {
            SourceLocation lBraceLoc = compoundBody->getLBracLoc();

            if (lBraceLoc.isValid()) {
                // Insert after the opening brace
                SourceLocation insertLoc = Lexer::getLocForEndOfToken(lBraceLoc, 0, sourceManager, LangOptions());
                SourceRange insertRange(insertLoc, insertLoc);

                std::string constructCall = "\n    this->state." + rangedFor.loopVarName +
                                            ".construct(*" + makeStateGetCall(rangedFor.beginVarName) + ");";

                int constructPriority = 20000 + static_cast<int>(rangedFor.index);
                globalReplacements.emplace_back(insertRange, constructCall, constructPriority);

                REWRITE_LOG() << "        Added construct insertion: '" << constructCall << "' after "
                        << lBraceLoc.printToString(sourceManager) << " (priority " << constructPriority << ")\n";
            } else {
                REWRITE_LOG() << "        ERROR: Invalid opening brace location\n";
            }
        } else {
            REWRITE_LOG() << "        ERROR: Loop body is not a compound statement\n";
        }
    }

    void collectRangedForLoopVarDestroy(const RangedForLoop &rangedFor) {
        REWRITE_LOG() << "      DEBUG: Collecting loop var destroy for ranged-for loop " << rangedFor.index << "\n";

        // Find the closing brace of the loop body
        const Stmt *body = rangedFor.stmt->getBody();
        if (auto *compoundBody = dyn_cast<CompoundStmt>(body)) {
            SourceLocation rBraceLoc = compoundBody->getRBracLoc();

            if (rBraceLoc.isValid()) {
                unsigned fileOffset = sourceManager.getFileOffset(rBraceLoc);

                std::string destroyCall = "    " + makeStateDestroyCall(rangedFor.loopVarName) + ";\n";

                // Use high priority to ensure it comes after regular variable destructors
                int destroyPriority = 30000 + static_cast<int>(rangedFor.index);

                ScopeEndReplacement replacement;
                replacement.replacement = destroyCall;
                replacement.priority = destroyPriority;

                scopeEndReplacements[fileOffset].push_back(replacement);

                REWRITE_LOG() << "        Added destroy insertion: '" << destroyCall << "' before "
                        << rBraceLoc.printToString(sourceManager) << " (priority " << destroyPriority
                        << ", file offset " << fileOffset << ")\n";
            } else {
                REWRITE_LOG() << "        ERROR: Invalid closing brace location\n";
            }
        } else {
            REWRITE_LOG() << "        ERROR: Loop body is not a compound statement\n";
        }
    }

    std::string generateExplicitLoopForm(const RangedForLoop &rangedFor) {
        REWRITE_LOG() << "\n=== GENERATING EXPLICIT LOOP FORM ===\n";
        REWRITE_LOG() << "      DEBUG: Generating explicit loop form for loop " << rangedFor.index << "\n";
        REWRITE_LOG() << "      Original ranged-for range: " << rangedFor.fullRange.getBegin().printToString(
                    sourceManager)
                << " to " << rangedFor.fullRange.getEnd().printToString(sourceManager) << "\n";

        // Get the loop body - keep it as-is for now, it will be processed by normal coroutine rewriting
        std::string loopBody = "";
        const Stmt *body = rangedFor.stmt->getBody();
        if (body) {
            SourceRange bodyRange = body->getSourceRange();
            CharSourceRange charRange = CharSourceRange::getTokenRange(bodyRange);
            loopBody = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();

            REWRITE_LOG() << "      Original loop body range: " << bodyRange.getBegin().printToString(sourceManager)
                    << " to " << bodyRange.getEnd().printToString(sourceManager) << "\n";
            REWRITE_LOG() << "      Original loop body text: '" << loopBody << "'\n";
            REWRITE_LOG() << "      Original loop body length: " << loopBody.length() << " characters\n";
        } else {
            REWRITE_LOG() << "      ERROR: No loop body found!\n";
        }

        std::string result = "{\n";
        REWRITE_LOG() << "      Building replacement text:\n";

        // Use external FOR_LOOP_HEADER macro for initialization
        std::string headerMacro = "  FOR_LOOP_HEADER(" + std::to_string(rangedFor.index) + ")\n";
        result += headerMacro;
        REWRITE_LOG() << "        1. Added header: '" << headerMacro.substr(0, headerMacro.length() - 1) << "'\n";

        // for (; this->state.__begin.get() != this->state.__end.get(); ++this->state.__begin.get()) {
        std::string forLine = "  for (; " + makeStateGetCall(rangedFor.beginVarName) + " != " +
                              makeStateGetCall(rangedFor.endVarName) + "; ++" + makeStateGetCall(rangedFor.beginVarName)
                              + ") {\n";
        result += forLine;
        REWRITE_LOG() << "        2. Added for line: '" << forLine.substr(0, forLine.length() - 1) << "'\n";

        // Construct the loop variable: this->state.loopVar.construct(*this->state.__begin.get());
        std::string loopVarConstruct = "    " + makeStateConstructCall(rangedFor.loopVarName, "*" + makeStateGetCall(
                                           rangedFor.beginVarName)) + ";\n";
        result += loopVarConstruct;
        REWRITE_LOG() << "        3. Added loop var construct: '" << loopVarConstruct.substr(
            0, loopVarConstruct.length() - 1) << "'\n";

        // Insert the loop body (without the outer braces if it's a compound statement)
        REWRITE_LOG() << "        4. Processing loop body...\n";
        if (loopBody.size() >= 2 && loopBody.front() == '{' && loopBody.back() == '}') {
            // Remove outer braces and add the inner content with proper indentation
            std::string innerBody = loopBody.substr(1, loopBody.size() - 2);
            REWRITE_LOG() << "           Detected compound body, extracting inner: '" << innerBody << "'\n";
            result += "    " + innerBody + "\n";
        } else {
            REWRITE_LOG() << "           Using body as-is: '" << loopBody << "'\n";
            result += "    " + loopBody + "\n";
        }

        // Add destructor call for loop variable at end of each iteration
        std::string loopVarDestroy = "    " + makeStateDestroyCall(rangedFor.loopVarName) + ";\n";
        result += loopVarDestroy;
        REWRITE_LOG() << "        4.5. Added loop var destroy: '" << loopVarDestroy.substr(
            0, loopVarDestroy.length() - 1) << "'\n";

        // Close the for loop - NOTE: FOR_LOOP_FOOTER will be added via global replacements
        std::string forClose = "  }\n"; // Close the for loop
        result += forClose;
        REWRITE_LOG() << "        5. Added for close: '" << forClose.substr(0, forClose.length() - 1) << "'\n";

        std::string scopeClose = "}"; // Close the outer scope
        result += scopeClose;
        REWRITE_LOG() << "        6. Added scope close: '" << scopeClose << "'\n";

        REWRITE_LOG() << "      FINAL Generated explicit loop text (length=" << result.length() << "):\n";
        REWRITE_LOG() << "=== START GENERATED TEXT ===\n" << result << "\n=== END GENERATED TEXT ===\n";
        REWRITE_LOG() << "=== END GENERATING EXPLICIT LOOP FORM ===\n\n";

        return result;
    }

private:
    void collectScopeEndReplacements() {
        REWRITE_LOG() << "  DEBUG: Processing scope end replacements for " << scopeEndReplacements.size() <<
                " positions\n";

        for (const auto &entry: scopeEndReplacements) {
            unsigned fileOffset = entry.first;
            const std::vector<ScopeEndReplacement> &replacements = entry.second;

            if (replacements.empty()) {
                continue;
            }

            REWRITE_LOG() << "    Processing " << replacements.size() << " replacements at file offset " << fileOffset
                    << "\n";

            // Sort replacements by priority
            std::vector<ScopeEndReplacement> sortedReplacements = replacements;
            std::sort(sortedReplacements.begin(), sortedReplacements.end());

            // Concatenate all destructor calls and append the closing brace
            std::string concatenatedReplacement;

            bool insertedBrace = false;
            auto insertBrace = [&concatenatedReplacement, &insertedBrace]() {
                if (!std::exchange(insertedBrace, true)) { concatenatedReplacement += "}";}
            };
            for (const auto &replacement: sortedReplacements) {
                if (replacement.insertAfterBrace) {
                    insertBrace();
                }
                concatenatedReplacement += replacement.replacement;
                REWRITE_LOG() << "      Added (priority " << replacement.priority << "): " << replacement.replacement.
                        substr(0, 40) << "...\n";
            }
            //concatenatedReplacement += "}"; // Append the closing brace

            // Convert file offset back to SourceLocation
            SourceLocation braceLocation = sourceManager.getLocForStartOfFile(sourceManager.getMainFileID()).
                    getLocWithOffset(fileOffset);
            SourceRange braceRange(braceLocation, braceLocation);

            // Use minimum priority among all replacements for this position
            int minPriority = sortedReplacements.empty() ? 0 : sortedReplacements[0].priority;

            insertBrace();// Re-add the brace after the concatenated replacement.
            globalReplacements.emplace_back(braceRange, concatenatedReplacement, minPriority);

            REWRITE_LOG() << "    Added concatenated replacement (" << concatenatedReplacement.length() << " chars) at "
                    << braceLocation.printToString(sourceManager) << " with priority " << minPriority << "\n";
        }

        REWRITE_LOG() << "  Processed " << scopeEndReplacements.size() << " scope end positions\n";
    }

public:
    void applyReplacements() {

        // Collect all insertion-style replacements into global vector
        collectCoroutineStatementReplacements();
        // Note: collectDestructorInsertions() is no longer needed since we use scopeEndReplacements
        // Note: ranged-for footer insertions will be collected in collectRangedForLoopReplacements

        // Apply range-style replacements first (declarations and references)
        collectRangeReplacements();

        // Apply ranged-for loop bulk replacements (this also collects footer insertions)
        collectRangedForLoopReplacements();

        // First process scope end replacements (destructors + closing braces)
        collectScopeEndReplacements();

        // Finally apply all insertion-style replacements with global sorting
        applyAllReplacements();
    }

    // DEPRECATED: This method is no longer used since we now use scopeEndReplacements
    /*
    void collectDestructorInsertions() {
        // This method has been replaced by collectScopeEndReplacements()
        // which properly handles concatenating destructor calls and appending closing braces
    }
    */

    void collectRangeReplacements() {
        REWRITE_LOG() <<
                "  DEBUG: Collecting range-style replacements (declarations and references) into global vector\n";

        // Add declaration replacements with priorities
        for (const auto &declReplacement: declReplacements) {
            // Use file offset as priority for declarations
            int declPriority = sourceManager.getFileOffset(declReplacement.first.getBegin());
            globalReplacements.emplace_back(declReplacement.first, declReplacement.second, declPriority);
        }

        // Add reference replacements with priorities
        for (const auto &refReplacement: refReplacements) {
            // Use file offset as priority for references
            int refPriority = sourceManager.getFileOffset(refReplacement.first.getBegin());
            globalReplacements.emplace_back(refReplacement.first, refReplacement.second, refPriority);
        }

        /*
        // Convert SourceRange replacements to multiple SourceLocation insertions if needed
        // For now, we'll apply range replacements immediately since they can't be easily converted to insertions
        // Sort by source location in reverse order to avoid invalidating positions
        std::sort(allRangeReplacements.begin(), allRangeReplacements.end(),
                 [&](const std::pair<SourceRange, std::string>& a, const std::pair<SourceRange, std::string>& b) {
                     return sourceManager.getFileOffset(a.first.getBegin()) > sourceManager.getFileOffset(b.first.getBegin());
                 });

        for (const auto& replacement : allRangeReplacements) {
            REWRITE_LOG() << "  Applying range replacement: " << replacement.second << "\n";
            rewriter.ReplaceText(replacement.first, replacement.second);
        }

        REWRITE_LOG() << "  Applied " << allRangeReplacements.size() << " range-style replacements\n";
        */
    }

    void applyAllReplacements() {
        REWRITE_LOG() << "\n=== APPLYING ALL REPLACEMENTS WITH PRIORITY ===\n";
        REWRITE_LOG() << "  DEBUG: Sorting and applying " << globalReplacements.size() << " replacements\n";

        // Sort ALL replacements by position first, then by priority
        std::stable_sort(globalReplacements.begin(), globalReplacements.end(),
                         [&](const std::tuple<SourceRange, std::string, int> &a,
                             const std::tuple<SourceRange, std::string, int> &b) {
                             unsigned offsetA = sourceManager.getFileOffset(std::get<0>(a).getBegin());
                             unsigned offsetB = sourceManager.getFileOffset(std::get<0>(b).getBegin());

                             if (offsetA != offsetB) {
                                 return offsetA > offsetB; // Reverse order for position safety
                             }

                             // Same position - sort by priority in ascending order
                             int priorityA = std::get<2>(a);
                             int priorityB = std::get<2>(b);
                             if (priorityA != priorityB) {
                                 return priorityA < priorityB;
                             }
                             return std::get<1>(a) < std::get<1>(b); // Sort by replacement text in ascending order
                         });
        // Deduplicate, such that the arbitrary redundant visits don't appear.
        globalReplacements.erase(std::unique(globalReplacements.begin(), globalReplacements.end()), globalReplacements.end());

        // Apply all replacements in the determined order
        std::optional<std::tuple<SourceRange, std::string, int> > buffer;
        for (size_t i = 0; i < globalReplacements.size(); ++i) {
            auto &replacement = globalReplacements[i];

            if (!buffer.has_value()) {
                buffer = std::move(replacement);
                continue;
            }

            // Check if this replacement affects the same source range
            if (std::get<0>(*buffer) != std::get<0>(replacement)) {
                // Different source range - apply the buffered replacement
                REWRITE_LOG() << "    [" << i - 1 << "] Applying at " << std::get<0>(*buffer).printToString(
                            sourceManager)
                        << " (priority " << std::get<2>(*buffer) << "): '" << std::get<1>(*buffer) << "'\n";
                rewriter.ReplaceText(std::get<0>(*buffer), std::get<1>(*buffer));
                buffer = std::move(replacement);
                continue;
            }

            // Same source range - combine the replacements (higher priority first)
            std::get<1>(*buffer) += std::get<1>(replacement);
            REWRITE_LOG() << "    [" << i << "] Combined with priority " << std::get<2>(replacement)
                    << ": '" << std::get<1>(replacement) << "'\n";
        }

        // Apply the final buffered replacement
        if (buffer.has_value()) {
            REWRITE_LOG() << "    [final] Applying at " << std::get<0>(*buffer).printToString(sourceManager)
                    << " (priority " << std::get<2>(*buffer) << "): '" << std::get<1>(*buffer) << "'\n";
            rewriter.ReplaceText(std::get<0>(*buffer), std::get<1>(*buffer));
        }

        REWRITE_LOG() << "  Applied " << globalReplacements.size() << " replacements with priority sorting\n";
        REWRITE_LOG() << "=== END APPLYING ALL REPLACEMENTS ===\n\n";
    }

    // Getter methods for accessing replacements without applying them
    const std::vector<std::pair<SourceRange, std::string> > &getDeclReplacements() const {
        return declReplacements;
    }

    const std::vector<std::pair<SourceRange, std::string> > &getRefReplacements() const {
        return refReplacements;
    }

    const std::vector<std::pair<SourceLocation, std::string> > &getDestructorInsertions() const {
        return destructorInsertions;
    }

    const std::vector<RangedForLoop> &getRangedForLoops() const {
        return rangedForLoops;
    }
};

class CoroutineRewriter : public RecursiveASTVisitor<CoroutineRewriter> {
private:
    const SourceManager &sourceManager;
    Rewriter &rewriter;
    clang::DiagnosticsEngine &diagnosticsEngine;
    const LangOptions &langOptions;
    ASTContext *astContext;
    std::vector<CoroutineInfo> coroutines;

    bool containsCoroutineKeywords(const Stmt *stmt) {
        if (!stmt) return false;

        if (isa<CoawaitExpr>(stmt) || isa<CoyieldExpr>(stmt) || isa<CoreturnStmt>(stmt)) {
            return true;
        }

        for (auto it = stmt->child_begin(); it != stmt->child_end(); ++it) {
            if (const Stmt *child = *it) {
                if (containsCoroutineKeywords(child)) {
                    return true;
                }
            }
        }
        return false;
    }

    bool containsTryCatchBlocks(const Stmt *stmt) {
        if (!stmt) return false;

        if (isa<CXXTryStmt>(stmt)) {
            return true;
        }

        for (auto it = stmt->child_begin(); it != stmt->child_end(); ++it) {
            if (const Stmt *child = *it) {
                if (containsTryCatchBlocks(child)) {
                    return true;
                }
            }
        }
        return false;
    }

    void collectLocalVariables(const Stmt *body, std::set<LocalVariable> &variables) {
        LocalVariableCollector collector(variables, sourceManager, *astContext);
        collector.TraverseStmt(const_cast<Stmt *>(body));
    }

    void collectFunctionParameters(const FunctionDecl *funcDecl, std::vector<FunctionParameter> &parameters) {
        REWRITE_LOG() << "  DEBUG: Collecting function parameters\n";
        
        for (const auto *param : funcDecl->parameters()) {
            FunctionParameter fp;
            fp.name = param->getNameAsString();
            fp.qualType = param->getType();
            
            // Get the type as string for debugging/logging
            PrintingPolicy policy(astContext->getLangOpts());
            fp.type = fp.qualType.getAsString(policy);
            
            parameters.push_back(fp);
            
            REWRITE_LOG() << "    Found parameter: " << fp.type << " " << fp.name << "\n";
        }
        
        REWRITE_LOG() << "  Found " << parameters.size() << " function parameters\n";
    }

    std::string replaceLastTemplateArgWithHandle(const std::string &returnType) {
        REWRITE_LOG() << "    DEBUG: replaceLastTemplateArgWithHandle input: '" << returnType << "'\n";

        // Find the template arguments by looking for < and >
        size_t openAngle = returnType.find('<');
        if (openAngle == std::string::npos) {
            REWRITE_LOG() << "    DEBUG: No template arguments found, returning unchanged\n";
            return returnType;
        }

        // Find the matching closing angle bracket
        size_t closeAngle = returnType.rfind('>');
        if (closeAngle == std::string::npos || closeAngle <= openAngle) {
            REWRITE_LOG() << "    DEBUG: Invalid template syntax, returning unchanged\n";
            return returnType;
        }

        // Extract the template arguments part
        std::string templateArgs = returnType.substr(openAngle + 1, closeAngle - openAngle - 1);
        REWRITE_LOG() << "    DEBUG: Template arguments: '" << templateArgs << "'\n";

        // Parse template arguments (simple comma splitting, ignoring nested templates for now)
        std::vector<std::string> args;
        size_t start = 0;
        int depth = 0;
        for (size_t i = 0; i <= templateArgs.length(); ++i) {
            if (i == templateArgs.length() || (templateArgs[i] == ',' && depth == 0)) {
                if (i > start) {
                    std::string arg = templateArgs.substr(start, i - start);
                    // Trim whitespace
                    size_t first = arg.find_first_not_of(" \t");
                    size_t last = arg.find_last_not_of(" \t");
                    if (first != std::string::npos && last != std::string::npos) {
                        arg = arg.substr(first, last - first + 1);
                    }
                    args.push_back(arg);
                }
                start = i + 1;
            } else if (templateArgs[i] == '<') {
                depth++;
            } else if (templateArgs[i] == '>') {
                depth--;
            }
        }

        REWRITE_LOG() << "    DEBUG: Parsed " << args.size() << " template arguments:\n";
        for (size_t i = 0; i < args.size(); ++i) {
            REWRITE_LOG() << "      [" << i << "]: '" << args[i] << "'\n";
        }

        if (args.empty()) {
            REWRITE_LOG() << "    DEBUG: No template arguments found after parsing\n";
            return returnType;
        }

        // Replace the last argument with Handle
        REWRITE_LOG() << "    DEBUG: Replacing last argument '" << args.back() << "' with 'Handle'\n";
        args.back() = "Handle";

        // Reconstruct the type
        std::string prefix = returnType.substr(0, openAngle + 1);
        std::string suffix = returnType.substr(closeAngle);

        std::string newTemplateArgs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) newTemplateArgs += ", ";
            newTemplateArgs += args[i];
        }

        std::string result = prefix + newTemplateArgs + suffix;
        REWRITE_LOG() << "    DEBUG: replaceLastTemplateArgWithHandle output: '" << result << "'\n";
        return result;
    }

    SourceLocation findStructInsertionPoint(const FunctionDecl *funcDecl) {
        REWRITE_LOG() << "DEBUG: findStructInsertionPoint called for function: " << funcDecl->getQualifiedNameAsString()
                << "\n";

        const Stmt *bodyStmt = funcDecl->getBody();
        if (!bodyStmt) {
            //REWRITE_LOG() << "DEBUG: Function has no body\n";
            return SourceLocation();
        }

        //REWRITE_LOG() << "DEBUG: Function has body, type: " << bodyStmt->getStmtClassName() << "\n";

        // Handle CoroutineBodyStmt wrapper
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            //REWRITE_LOG() << "DEBUG: Body is CoroutineBodyStmt, getting inner body\n";
            bodyStmt = coroBody->getBody();
            if (!bodyStmt) {
                //REWRITE_LOG() << "DEBUG: CoroutineBodyStmt has no inner body\n";
                return SourceLocation();
            }
            //REWRITE_LOG() << "DEBUG: Inner body type: " << bodyStmt->getStmtClassName() << "\n";
        }

        if (auto *body = dyn_cast<CompoundStmt>(bodyStmt)) {
            //REWRITE_LOG() << "DEBUG: Body is CompoundStmt\n";
            SourceLocation lbracLoc = body->getLBracLoc();
            //REWRITE_LOG() << "DEBUG: getLBracLoc() returned location, checking validity\n";

            if (lbracLoc.isValid()) {
                //REWRITE_LOG() << "DEBUG: lbracLoc is valid, calling Lexer::getLocForEndOfToken\n";
                SourceLocation endLoc = Lexer::getLocForEndOfToken(lbracLoc, 0, sourceManager, langOptions);
                if (endLoc.isValid()) {
                    //REWRITE_LOG() << "DEBUG: Successfully found insertion point\n";
                    return endLoc;
                } else {
                    //REWRITE_LOG() << "DEBUG: Lexer::getLocForEndOfToken returned invalid location\n";
                }
            } else {
                REWRITE_LOG() << "DEBUG: lbracLoc is invalid\n";
            }
        } else {
            REWRITE_LOG() << "DEBUG: Final body is not CompoundStmt, type: " << bodyStmt->getStmtClassName() << "\n";
        }

        REWRITE_LOG() << "DEBUG: Returning invalid SourceLocation\n";
        return SourceLocation();
    }

    std::string generateCoroImplStruct(const CoroutineInfo &coro) {
        // Extract the return type from the coroutine function
        std::string returnType = "auto"; // Default fallback
        if (coro.function) {
            QualType retType = coro.function->getReturnType();

            // Use canonical type to handle implicitly defaulted template arguments
            QualType canonicalType = retType.getCanonicalType();

            PrintingPolicy policy(astContext->getLangOpts());
            policy.SuppressScope = false;
            policy.PrintCanonicalTypes = true; // Print canonical types to see defaulted args

            std::string originalReturnType = canonicalType.getAsString(policy);
            REWRITE_LOG() << "  DEBUG: Original canonical return type: " << originalReturnType << "\n";

            // Also print the non-canonical version for comparison
            std::string nonCanonicalType = retType.getAsString(policy);
            REWRITE_LOG() << "  DEBUG: Non-canonical return type: " << nonCanonicalType << "\n";

            // Replace last template argument with HANDLE (work on canonical type)
            returnType = replaceLastTemplateArgWithHandle(originalReturnType);
            REWRITE_LOG() << "  DEBUG: Modified return type: " << returnType << "\n";
        }
        std::string structCode = "\n  // _coro_storage and CoroImpl assumed to be available in global namespace\n";

        /*
        // Commented out - assumed to be available globally
        structCode += "  template<typename T>\n";
        structCode += "  struct _coro_storage {\n";
        structCode += "    alignas(T) char buffer[sizeof(T)];\n";
        structCode += "    bool constructed = false;\n";
        structCode += "\n";
        structCode += "    template<typename... Args>\n";
        structCode += "    void construct(Args&&... args) {\n";
        structCode += "      new(buffer) T(std::forward<Args>(args)...);\n";
        structCode += "      constructed = true;\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    void destroy() {\n";
        structCode += "      if (constructed) {\n";
        structCode += "        reinterpret_cast<T*>(buffer)->~T();\n";
        structCode += "        constructed = false;\n";
        structCode += "      }\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    T& get() {\n";
        structCode += "      return *reinterpret_cast<T*>(buffer);\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    const T& get() const {\n";
        structCode += "      return *reinterpret_cast<const T*>(buffer);\n";
        structCode += "    }\n";
        structCode += "\n";
        structCode += "    ~_coro_storage() {\n";
        structCode += "      destroy();\n";
        structCode += "    }\n";
        structCode += "  };\n";
        structCode += "\n";
        */

        // Generate the data structure
        structCode += "  struct _detail_coro_impl {\n";

        // Debug: Print all variables that will be added to the struct
        REWRITE_LOG() << "  DEBUG: generateCoroImplStruct - Processing " << coro.localVariables.size() <<
                " variables:\n";
        for (const auto &var: coro.localVariables) {
            REWRITE_LOG() << "    Variable: " << var.type << " " << var.name << "\n";
        }

        // Add function parameters first
        if (!coro.parameters.empty()) {
            structCode += "    // Function parameters\n";
            for (const auto &param : coro.parameters) {
                std::string paramType = "decltype(" + param.name + ")";
                structCode += "    " + paramType + " " + param.name + ";\n";
                REWRITE_LOG() << "    Added parameter to struct: " << paramType << " " << param.name << "\n";
            }
            structCode += "\n";
        }

        // Add all local variables (including ranged-for variables)
        if (coro.localVariables.empty()) {
            structCode += "    // No local variables found in this coroutine\n";
        } else {
            structCode += "    // Local variables (including ranged-for loop variables)\n";
            for (const auto &var: coro.localVariables) {
                structCode += "    _coro_storage<" + var.referenceType + ", " + (var.isOwning ? std::string{"true"} : std::string{"false"}) + "> " + var.name +
                        ";\n";
                /*
                REWRITE_LOG() << "  DEBUG: Added to struct: _coro_storage<" << var.isOwning << ", " << var.
                        referenceType << "> " << var.name
                        << " (original type: " << var.type << ", is reference: " << (var.isReference ? "yes" : "no") <<
                        ");\n";
                        */
            }
        }

        structCode += "  };\n\n";

        // Add typedef to avoid comma issues in macro call
        structCode += "  using _ActualCoroType = " + returnType + ";\n";

        // Generate the COROUTINE_HEADER macro call
        // The coroutine body will follow immediately after this
        structCode += "  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl) ";

        return structCode;
    }

public:
    CoroutineRewriter(Rewriter &rewr, const SourceManager &SM, DiagnosticsEngine &diag, const LangOptions &langOpts)
        : sourceManager(SM), rewriter(rewr), diagnosticsEngine(diag), langOptions(langOpts), astContext(nullptr) {
    }

    void setASTContext(ASTContext &ctx) {
        astContext = &ctx;
    }

    bool VisitFunctionDecl(FunctionDecl *funcDecl) {
        if (!funcDecl->hasBody()) {
            return true;
        }

        auto fileId = sourceManager.getFileID(funcDecl->getLocation());
        if (fileId != sourceManager.getMainFileID()) {
            return true;
        }

        if (containsCoroutineKeywords(funcDecl->getBody())) {
            // Check if the coroutine contains try-catch blocks - skip if it does
            if (containsTryCatchBlocks(funcDecl->getBody())) {
                REWRITE_LOG() << "Skipping coroutine " << funcDecl->getQualifiedNameAsString() 
                              << " because it contains try-catch blocks (not yet supported)\n";
                return true; // Skip this coroutine
            }
            CoroutineInfo coro;
            coro.function = funcDecl;

            REWRITE_LOG() << "Processing coroutine: " << funcDecl->getQualifiedNameAsString() << "\n";

            collectLocalVariables(funcDecl->getBody(), coro.localVariables);
            collectFunctionParameters(funcDecl, coro.parameters);

            coro.insertionPoint = findStructInsertionPoint(funcDecl);
            if (coro.insertionPoint.isInvalid()) {
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Warning,
                    "Could not find insertion point for _detail_coro_impl struct");
                diagnosticsEngine.Report(funcDecl->getLocation(), diagID);
                coro.hasError = true;

                // Output the struct code to console as fallback
                std::string structCode = generateCoroImplStruct(coro);
                REWRITE_LOG() << "WARNING: Could not insert struct, here's what would have been inserted:\n";
                REWRITE_LOG() << "========== STRUCT CODE ==========\n";
                REWRITE_LOG() << structCode;
                REWRITE_LOG() << "==================================\n";
            }

            coroutines.push_back(coro);

            REWRITE_LOG() << "  Found " << coro.localVariables.size() << " local variables\n";

            if (const auto *methodDecl = dyn_cast<CXXMethodDecl>(funcDecl)) {
                REWRITE_LOG() << "  Type: Member function of " << methodDecl->getParent()->getQualifiedNameAsString() <<
                        "\n";
            } else {
                REWRITE_LOG() << "  Type: Free function\n";
            }
        }

        return true;
    }

    void updateFunctionReturnType(const CoroutineInfo &coro) {
        REWRITE_LOG() << "Updating function return type for: " << coro.function->getQualifiedNameAsString() << "\n";

        if (!coro.function) {
            REWRITE_LOG() << "  ERROR: No function to update\n";
            return;
        }

        // Get the return type location
        SourceRange returnTypeRange = coro.function->getReturnTypeSourceRange();
        if (returnTypeRange.isInvalid()) {
            REWRITE_LOG() << "  ERROR: Invalid return type source range\n";
            return;
        }

        // Use the same logic as generateCoroImplStruct to get the canonical type
        QualType retType = coro.function->getReturnType();
        QualType canonicalType = retType.getCanonicalType();

        PrintingPolicy policy(astContext->getLangOpts());
        policy.SuppressScope = false;
        policy.PrintCanonicalTypes = true;

        std::string originalReturnType = canonicalType.getAsString(policy);

        // Replace last template argument with Handle (same as typedef)
        std::string modifiedReturnType = replaceLastTemplateArgWithHandle(originalReturnType);

        REWRITE_LOG() << "  DEBUG: Original canonical return type: " << originalReturnType << "\n";
        REWRITE_LOG() << "  DEBUG: Modified return type: " << modifiedReturnType << "\n";

        if (originalReturnType != modifiedReturnType) {
            rewriter.ReplaceText(returnTypeRange, modifiedReturnType);
            REWRITE_LOG() << "  Updated function return type\n";
        } else {
            REWRITE_LOG() << "  Return type unchanged (no template arguments found)\n";
        }
    }

    void performRewrites() {
        for (const auto &coro: coroutines) {
            if (coro.hasError) {
                continue;
            }

            if (coro.insertionPoint.isValid()) {
                // Update the function's return type before rewriting the body
                updateFunctionReturnType(coro);

                // First rewrite the coroutine body (this adds ranged-for variables to localVariables)
                rewriteCoroutineBody(coro);

                // THEN generate the struct with all variables (including ranged-for vars)
                std::string structCode = generateCoroImplStruct(coro);
                rewriter.InsertTextBefore(coro.insertionPoint, structCode);
                REWRITE_LOG() << "Inserted _detail_coro_impl struct into "
                        << coro.function->getQualifiedNameAsString() << "\n";
            }
        }
    }

    void rewriteCoroutineBody(const CoroutineInfo &coro) {
        REWRITE_LOG() << "Rewriting coroutine body for: " << coro.function->getQualifiedNameAsString() << "\n";

        const Stmt *bodyStmt = coro.function->getBody();
        if (!bodyStmt) {
            return;
        }

        // Handle CoroutineBodyStmt wrapper
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            bodyStmt = coroBody->getBody();
        }

        if (bodyStmt) {
            // First pass: collect ranged-for loops only (don't apply any replacements yet)
            CoroutineBodyRewriter initialRewriter(coro.localVariables, rewriter, sourceManager);
            initialRewriter.TraverseStmt(const_cast<Stmt *>(bodyStmt));

            // Get the ranged-for loops and add them to the coroutine info for struct generation
            const auto &rangedForLoops = initialRewriter.getRangedForLoops();
            const_cast<CoroutineInfo &>(coro).rangedForLoops = rangedForLoops;
            REWRITE_LOG() << "  DEBUG: Found " << rangedForLoops.size() << " ranged-for loops\n";

            // Add ranged-for variables to the local variables set so they appear in the struct
            for (const auto &rangedFor: rangedForLoops) {
                // Add the actual loop variable to the struct
                LocalVariable loopVar;
                loopVar.name = rangedFor.loopVarName;
                loopVar.type = rangedFor.loopVarType;
                
                // For ranged-for loop variables, we need to determine storage based on the loop variable type
                // Since there's no explicit initializer, we analyze the type directly
                // The conceptual initializer would be `*iterator`, which is typically an lvalue
                
                loopVar.isReference = (loopVar.type.find("&") != std::string::npos);

                loopVar.referenceType = loopVar.type;
                loopVar.isOwning = !loopVar.isReference;
                loopVar.location = rangedFor.fullRange.getBegin();
                loopVar.priority = sourceManager.getFileOffset(loopVar.location); // Use file offset as priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(loopVar);

                LocalVariable rangeVar;
                rangeVar.name = rangedFor.rangeVarName;
                std::tie(rangeVar.type, rangeVar.isOwning) =
                        getTypeForAutoRefRefVariable(rangedFor.rangeExpr, *astContext);
                rangeVar.referenceType = rangeVar.type + " &";
                rangeVar.isReference = false;
                rangeVar.location = rangedFor.fullRange.getBegin();
                rangeVar.priority = sourceManager.getFileOffset(rangeVar.location) + 1; // Slightly later priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(rangeVar);

                auto addBeginAndEndVar = [&](const std::string& beginOrEnd, const std::string& name, int priority) {
                    LocalVariable beginVar;
                    beginVar.name = name;
                    beginVar.type = "std::decay_t<decltype(std::declval<" + rangeVar.referenceType + ">()."+ beginOrEnd + "())>";
                    beginVar.isOwning = true; // Iterator types are typically not references
                    beginVar.referenceType = beginVar.type + " &";
                    beginVar.isReference = false;
                    beginVar.location = rangedFor.fullRange.getBegin();
                    beginVar.priority = sourceManager.getFileOffset(beginVar.location) + priority;
                    const_cast<CoroutineInfo &>(coro).localVariables.insert(beginVar);
                };

                addBeginAndEndVar("begin", rangedFor.beginVarName, 2);
                addBeginAndEndVar("end", rangedFor.endVarName, 3);
                /*
                LocalVariable beginVar;
                beginVar.name = rangedFor.beginVarName;
                beginVar.type = "decltype(begin(std::declval<decltype(" + rangedFor.rangeExpr + ")>()))";
                beginVar.isOwning = true; // Iterator types are typically not references
                beginVar.referenceType = beginVar.type + " &";
                beginVar.isReference = false;
                beginVar.location = rangedFor.fullRange.getBegin();
                beginVar.priority = sourceManager.getFileOffset(beginVar.location) + 2; // Slightly later priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(beginVar);

                LocalVariable endVar;
                endVar.name = rangedFor.endVarName;
                endVar.type = "decltype(end(std::declval<decltype(" + rangedFor.rangeExpr + ")>()))";
                endVar.isOwning = true; // Iterator types are typically not references
                endVar.referenceType = endVar.type + " &";
                endVar.isReference = false;
                endVar.location = rangedFor.fullRange.getBegin();
                endVar.priority = sourceManager.getFileOffset(endVar.location) + 3; // Even later priority
                const_cast<CoroutineInfo &>(coro).localVariables.insert(endVar);

                REWRITE_LOG() << "    Added ranged-for variables to struct: " << rangedFor.loopVarName
                        << ", " << rangedFor.rangeVarName << ", " << rangedFor.beginVarName
                        << ", " << rangedFor.endVarName << "\n";
                        */
            }

            // Apply only ranged-for loop replacements first (this transforms ranged-for to explicit loops)
            //REWRITE_LOG() << "  DEBUG: Applying ranged-for loop transformations\n";
            //initialRewriter.applyRangedForLoopReplacements();

            // Second pass: create new rewriter with all variables (including ranged-for vars)
            // This will process the explicit loops with proper variable rewriting
            REWRITE_LOG() << "  DEBUG: Creating final rewriter with " << coro.localVariables.size() << " variables:\n";
            for (const auto &var: coro.localVariables) {
                REWRITE_LOG() << "    Variable: " << var.type << " " << var.name << "\n";
            }

            CoroutineBodyRewriter finalRewriter(coro.localVariables, rewriter, sourceManager);
            finalRewriter.TraverseStmt(const_cast<Stmt *>(bodyStmt));

            // Apply all the variable rewrites in place (construct/destroy calls, get() access)
            REWRITE_LOG() << "  DEBUG: Applying all variable transformations in place\n";

            // Now wrap the transformed code with run() method braces
            REWRITE_LOG() << "  DEBUG: Wrapping transformed code with run() method\n";
            wrapBodyWithRunMethod(coro, finalRewriter);
            finalRewriter.applyReplacements();

            REWRITE_LOG() << "Completed body rewriting for: " << coro.function->getQualifiedNameAsString() << "\n";
        }
    }

    std::string getTransformedBodyText(const Stmt *bodyStmt, CoroutineBodyRewriter &bodyRewriter) {
        // Get the compound statement body
        if (auto *compoundStmt = dyn_cast<CompoundStmt>(bodyStmt)) {
            // Get the inner part (without the outer braces)
            SourceLocation startLoc = Lexer::getLocForEndOfToken(compoundStmt->getLBracLoc(), 0, sourceManager,
                                                                 langOptions);
            SourceLocation endLoc = compoundStmt->getRBracLoc();

            SourceRange innerRange(startLoc, endLoc);
            CharSourceRange charRange = CharSourceRange::getCharRange(innerRange);
            std::string bodyText = Lexer::getSourceText(charRange, sourceManager, langOptions).str();

            // Apply transformations in memory
            std::string transformedText = applyTransformationsInMemory(bodyText, bodyRewriter);

            return transformedText;
        }
        return "";
    }

    std::string applyTransformationsInMemory(const std::string &originalText, CoroutineBodyRewriter &bodyRewriter) {
        std::string result = originalText;

        // Get all the replacements that would have been made
        const auto &declReplacements = bodyRewriter.getDeclReplacements();
        const auto &refReplacements = bodyRewriter.getRefReplacements();
        const auto &destructorInsertions = bodyRewriter.getDestructorInsertions();

        // For now, we'll do simple text-based transformations
        // In a full implementation, we would need more sophisticated source location mapping

        REWRITE_LOG() << "    DEBUG: Original text length: " << originalText.length() << "\n";
        REWRITE_LOG() << "    DEBUG: Found " << declReplacements.size() << " declaration replacements\n";
        REWRITE_LOG() << "    DEBUG: Found " << refReplacements.size() << " reference replacements\n";
        REWRITE_LOG() << "    DEBUG: Found " << destructorInsertions.size() << " destructor insertions\n";

        // Simple regex-based replacement for now
        // TODO: Implement proper source location mapping for precise replacements

        return result;
    }

    void replaceEntireBodyWithStateMachine(const CoroutineInfo &coro) {
        REWRITE_LOG() << "  DEBUG: Replacing entire body with state machine instantiation\n";

        const Stmt *originalBody = coro.function->getBody();
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(originalBody)) {
            originalBody = coroBody->getBody();
        }

        if (auto *compoundStmt = dyn_cast<CompoundStmt>(originalBody)) {
            SourceLocation lbraceLoc = compoundStmt->getLBracLoc();
            SourceLocation rbraceLoc = compoundStmt->getRBracLoc();

            REWRITE_LOG() << "  DEBUG: LBrace location: " << lbraceLoc.printToString(sourceManager) << "\n";
            REWRITE_LOG() << "  DEBUG: RBrace location: " << rbraceLoc.printToString(sourceManager) << "\n";

            // Validate basic locations
            if (lbraceLoc.isInvalid() || rbraceLoc.isInvalid()) {
                REWRITE_LOG() << "  ERROR: Invalid brace locations\n";
                return;
            }

            // Get the full range including braces
            SourceRange fullRange = compoundStmt->getSourceRange();
            REWRITE_LOG() << "  DEBUG: Full compound statement range: "
                    << fullRange.getBegin().printToString(sourceManager) << " to "
                    << fullRange.getEnd().printToString(sourceManager) << "\n";

            // Get the original text to see what we're working with
            CharSourceRange charRange = CharSourceRange::getCharRange(fullRange);
            std::string originalText = Lexer::getSourceText(charRange, sourceManager, langOptions).str();
            REWRITE_LOG() << "  DEBUG: Original compound statement text: '" << originalText << "'\n";
            REWRITE_LOG() << "  DEBUG: Original text length: " << originalText.length() << "\n";

            // Calculate file offsets for debugging
            unsigned startOffset = sourceManager.getFileOffset(fullRange.getBegin());
            unsigned endOffset = sourceManager.getFileOffset(fullRange.getEnd());
            REWRITE_LOG() << "  DEBUG: File offset range: " << startOffset << " to " << endOffset
                    << " (length: " << (endOffset - startOffset + 1) << ")\n";

            // Try a safer approach: replace the entire compound statement
            std::string stateMachineCall =
                    "{\n    _detail_coro_statemachine_impl stateMachine;\n    stateMachine.run();\n  }";

            REWRITE_LOG() << "  DEBUG: About to replace with: '" << stateMachineCall << "'\n";
            REWRITE_LOG() << "  DEBUG: Replacement length: " << stateMachineCall.length() << "\n";

            // Use the full range instead of trying to calculate inner range
            rewriter.ReplaceText(fullRange, stateMachineCall);
            /*
                try {
                rewriter.ReplaceText(fullRange, stateMachineCall);
                REWRITE_LOG() << "  DEBUG: Successfully replaced compound statement\n";
            } catch (...) {
                REWRITE_LOG() << "  ERROR: Exception during replacement\n";
            }
            */
        }
    }


    void wrapBodyWithRunMethod(const CoroutineInfo &coro, CoroutineBodyRewriter &body_rewriter) {
        REWRITE_LOG() << "  DEBUG: Adding closing braces after coroutine body for: " << coro.function->
                getQualifiedNameAsString() << "\n";

        const Stmt *bodyStmt = coro.function->getBody();
        if (!bodyStmt) {
            REWRITE_LOG() << "  ERROR: Function has no body to wrap\n";
            return;
        }

        // Handle CoroutineBodyStmt wrapper
        if (auto *coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            bodyStmt = coroBody->getBody();
        }

        if (auto *compoundStmt = dyn_cast<CompoundStmt>(bodyStmt)) {
            SourceLocation rbraceLoc = compoundStmt->getRBracLoc();

            REWRITE_LOG() << "  DEBUG: Found compound statement closing brace\n";
            REWRITE_LOG() << "  DEBUG: RBrace location: " << rbraceLoc.printToString(sourceManager) << "\n";

            if (rbraceLoc.isValid()) {
                // Add COROUTINE_FOOTER to the scope end replacements system
                // This ensures it gets properly ordered with destructor calls
                unsigned fileOffset = sourceManager.getFileOffset(rbraceLoc);
                
                ScopeEndReplacement replacement;
                
                // Build parameter list for COROUTINE_FOOTER
                std::string paramList;
                if (!coro.parameters.empty()) {
                    for (size_t i = 0; i < coro.parameters.size(); ++i) {
                        if (i > 0) paramList += ", ";
                        paramList += coro.parameters[i].name;
                    }
                    replacement.replacement = "COROUTINE_FOOTER(" + paramList + ")\n";
                } else {
                    replacement.replacement = "COROUTINE_FOOTER\n";
                }
                
                // Use maximum priority to ensure COROUTINE_FOOTER comes after all destructors
                replacement.priority = std::numeric_limits<int>::max();
                
                body_rewriter.scopeEndReplacements[fileOffset].push_back(replacement);
                
                REWRITE_LOG() << "  DEBUG: Added COROUTINE_FOOTER to scope end replacements with maximum priority (" 
                              << replacement.priority << ") at file offset " << fileOffset << "\n";
                REWRITE_LOG() << "  DEBUG: COROUTINE_FOOTER replacement text: '" << replacement.replacement << "'\n";

                REWRITE_LOG() << "  DEBUG: Successfully added COROUTINE_FOOTER to scope end system\n";
            } else {
                REWRITE_LOG() << "  ERROR: Invalid closing brace location for compound statement\n";
            }
        } else {
            REWRITE_LOG() << "  ERROR: Body is not a compound statement, cannot add closing braces\n";
        }
    }

    const std::vector<CoroutineInfo> &getCoroutines() const {
        return coroutines;
    }
};

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
