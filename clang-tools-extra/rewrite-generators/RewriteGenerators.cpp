//
// Created by kalmbacj on 2025-09-09.
//

#include <streambuf>
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
#include "llvm/IR/InlineAsm.h"
using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;


class NullBuffer : public std::streambuf {
protected:
    // Discard characters by pretending to succeed
    int overflow(int c) override { return traits_type::not_eof(c); }
};

// Create a global or local instance
inline std::ostream& null_stream() {
    static NullBuffer nullBuffer;
    static std::ostream nullStream(&nullBuffer);
    return nullStream;
}

// Logging macro for fine-grained control
#define REWRITE_LOG() null_stream()
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

// Helper function to generate CO_BRACED_INIT prefix  
static std::string makeBracedInitPrefix(const std::string &varName, bool isOwning) {
    if (isOwning) {
        return "CO_BRACED_INIT_OWNING(" + varName + ", ";
    } else {
        return "CO_BRACED_INIT(" + varName + ", ";
    }
}

// Helper function to generate CO_PAREN_INIT prefix
static std::string makeParenInitPrefix(const std::string &varName, bool isOwning) {
    if (isOwning) {
        return "CO_PAREN_INIT_OWNING(" + varName + ", ";
    } else {
        return "CO_PAREN_INIT(" + varName + ", ";
    }
}

static llvm::cl::OptionCategory MyToolCategory("coroutine-rewriter");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("\nRewrites C++20 coroutines to C++17 compatible state machines.\n");


struct RangedForLoop {
    const CXXForRangeStmt *stmt;
    std::string loopVarName;
    std::string loopVarType;
    Expr *rangeExpr;
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

struct TryCatchBlock {
    const CXXTryStmt *tryStmt;
    unsigned index;
    SourceLocation tryKeywordLoc;
    SourceLocation tryBlockStart;  // Opening brace of try block
    SourceLocation tryBlockEnd;    // Closing brace of try block
    std::vector<std::string> catchClauses;  // Transformed catch clause bodies
    SourceLocation catchEnd;       // End of all catch clauses
    std::vector<std::string> variablesInTryBlock;  // Variables declared inside this try block, in reverse order
};

struct TemporaryInfo {
    const MaterializeTemporaryExpr *expr; // The MaterializeTemporaryExpr node
    std::string tempVarName;             // e.g., temp_1_0
    QualType type;                       // Type of the temporary
    SourceLocation constructLoc;         // Where to insert the CO_INIT macro
    std::string initArgs;                // Arguments for the initialization
    bool isBracedInit;                   // true for CO_BRACED_INIT_OWNING, false for CO_PAREN_INIT_OWNING
};

struct CoroutineStatement {
    enum Type { YIELD, AWAIT, RETURN };

    Type type;
    const Stmt *stmt; // CoawaitExpr*, CoyieldExpr*, or CoreturnStmt*
    const Expr *operand; // The expression being yielded/awaited/returned
    unsigned index;
    SourceLocation keywordLoc;
    SourceLocation operandStart;
    SourceLocation operandEnd;
    bool needsBuffering = false; // True if operand is a temporary (not used for RETURN)
    QualType bufferType; // Type for the buffer when needsBuffering is true
    std::vector<std::string> aliveVariables; // Variables alive at this suspension point, in reverse order of declaration
    std::vector<TemporaryInfo> temporaries; // Subexpression temporaries that need lifetime extension
};


struct CoroutineInfo {
    const FunctionDecl *function;
    std::set<LocalVariable> localVariables;
    std::vector<RangedForLoop> rangedForLoops; // Add ranged-for loop info
    std::vector<TryCatchBlock> tryCatchBlocks; // Add try-catch block info
    std::vector<CoroutineStatement> coroutineStatements; // Add coroutine suspension points
    std::vector<FunctionParameter> parameters; // Function parameters
    SourceLocation insertionPoint;
    bool hasError = false;

    // Member function information
    bool isMemberFunction = false;
    bool isConstMemberFunction = false;
    std::string className;

    // Lambda information
    bool isLambda = false;
    const LambdaExpr *lambdaExpr = nullptr;

    // Temporary type information for yield buffer
    std::vector<QualType> yieldedOrAwaitedTemporaries;

    // Mapping from variable declaration location to member name (for handling shadowing)
    std::map<SourceLocation, std::string> declLocationToMemberName;
};


struct ScopeInfo {
    const CompoundStmt *compoundStmt;
    std::vector<std::string> variablesInScope;
    SourceLocation scopeEnd;

    // Loop tracking information
    bool isLoopScope = false;
    bool isLoopBodyScope = false;  // true if this is the body of a loop (not the header/init)
    const Stmt *loopStmt = nullptr;  // The loop statement this scope belongs to
};

struct ScopeEndReplacement {
    std::string replacement;
    int priority;
    bool insertAfterBrace = false;

    bool operator<(const ScopeEndReplacement &other) const {
        return insertAfterBrace != other.insertAfterBrace ? other.insertAfterBrace : priority < other.priority;
    }
};

// The bool means `true` for replace, `false` for insert after the beginning.
using Replacement = std::tuple<SourceRange, std::string, bool>;

// Helper visitor to collect MaterializeTemporaryExpr nodes from an expression
class TemporaryCollector : public RecursiveASTVisitor<TemporaryCollector> {
private:
    std::vector<const MaterializeTemporaryExpr*> &temporaries;

public:
    explicit TemporaryCollector(std::vector<const MaterializeTemporaryExpr*> &temps)
        : temporaries(temps) {}

    bool VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr *matTemp) {
        temporaries.push_back(matTemp);
        return true;
    }
};

// Helper visitor to collect variable references with their declaration locations
class VariableReferenceCollector : public RecursiveASTVisitor<VariableReferenceCollector> {
private:
    std::vector<std::pair<SourceRange, SourceLocation>> &references; // (reference location, decl location)
    const std::set<std::string> &variableNames;

public:
    VariableReferenceCollector(std::vector<std::pair<SourceRange, SourceLocation>> &refs,
                               const std::set<std::string> &varNames)
        : references(refs), variableNames(varNames) {}

    bool VisitDeclRefExpr(DeclRefExpr *declRef) {
        if (auto *varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
            std::string varName = varDecl->getNameAsString();
            if (variableNames.count(varName)) {
                SourceRange refRange = declRef->getSourceRange();
                SourceLocation declLoc = varDecl->getLocation();
                references.push_back({refRange, declLoc});
            }
        }
        return true;
    }
};

class CoroutineBodyRewriter : public RecursiveASTVisitor<CoroutineBodyRewriter> {
private:
    const std::set<LocalVariable> &localVariables;
    Rewriter &rewriter;
    const SourceManager &sourceManager;
    CoroutineInfo &coroutineInfo;
    ASTContext *astContext;
    std::set<std::string> variableNames;
    std::vector<Replacement> declReplacements;
    std::vector<Replacement> refReplacements;
    std::set<SourceLocation> processedDeclarations;
    std::set<SourceLocation> processedThisExpressions;
    std::vector<ScopeInfo> scopeStack;
    std::vector<const Stmt*> currentLoopStack;
    std::vector<std::pair<SourceLocation, std::string> > destructorInsertions;
    std::vector<CoroutineStatement> coroutineStatements;
    unsigned nextCoroStatementIndex;
    std::vector<RangedForLoop> rangedForLoops;
    unsigned nextRangedForIndex;
    std::vector<TryCatchBlock> tryCatchBlocks;
    unsigned nextTryCatchIndex;
    std::vector<unsigned> currentTryBlockStack;  // Stack of try block indices we're currently inside

    // Member function information
    bool isMemberFunction;
    [[maybe_unused]] const CXXRecordDecl *classDecl;

    // Map from variable declaration location to member name (for handling shadowing)
    std::map<SourceLocation, std::string> declLocationToMemberName;

    // Map from closing brace position to all replacements that should happen at that position

    // Global replacement vector for ALL types of replacements (SourceRange, replacement_string, priority)
public:
    std::map<unsigned, std::vector<ScopeEndReplacement> > scopeEndReplacements;
    // The int is the priority, the `bool` means `true` for replace.
    std::vector<std::tuple<SourceRange, std::string, int, bool> > globalReplacements;

public:
    CoroutineBodyRewriter(const std::set<LocalVariable> &vars, Rewriter &rewr, const SourceManager &SM,
                          CoroutineInfo &coroInfo, ASTContext &astCtx, bool isMember = false, const CXXRecordDecl *classRecord = nullptr)
        : localVariables(vars), rewriter(rewr), sourceManager(SM), coroutineInfo(coroInfo), astContext(&astCtx), nextCoroStatementIndex(1), nextRangedForIndex(0), nextTryCatchIndex(0),
          isMemberFunction(isMember), classDecl(classRecord) {
        // Create a set of variable names for quick lookup
        for (const auto &var: localVariables) {
            variableNames.insert(var.name);
        }

        // Build the mapping from declaration location to member name BEFORE AST traversal
        buildDeclLocationMapping();
    }

    // Build the mapping from variable declaration locations to member names
    // This handles shadowing by renaming variables with the same name in different scopes
    void buildDeclLocationMapping() {
        std::set<SourceLocation> addedDeclLocations; // Track which declarations we've already processed
        std::map<std::string, int> variableNameCounts; // Track count of each variable name for shadowing

        for (const auto &var : localVariables) {
            // Skip if we've already processed this exact variable declaration
            if (addedDeclLocations.count(var.location) > 0) {
                continue;
            }
            addedDeclLocations.insert(var.location);

            // Determine the member name (with suffix for shadowed variables)
            std::string memberName = var.name;
            int count = variableNameCounts[var.name]++;
            if (count > 0) {
                memberName = var.name + "_shadow_" + std::to_string(count);
            }

            // Store mapping in both places:
            // 1. Local member for use during AST traversal
            declLocationToMemberName[var.location] = memberName;
            // 2. CoroutineInfo for sharing with other classes (like CoroutineRewriter)
            coroutineInfo.declLocationToMemberName[var.location] = memberName;
        }
    }

    // Track compound statements (scopes) - using Traverse for proper pre/post hooks
    bool TraverseCompoundStmt(CompoundStmt *compoundStmt) {
        // ===== PRE-TRAVERSAL (before children are visited) =====
        REWRITE_LOG() << "  DEBUG: Entering scope (CompoundStmt)\n";

        // Create scope info
        ScopeInfo scope;
        scope.compoundStmt = compoundStmt;
        scope.scopeEnd = compoundStmt->getRBracLoc();

        // Check if this compound statement is part of a loop
        if (!currentLoopStack.empty()) {
            scope.isLoopScope = true;
            scope.loopStmt = currentLoopStack.back();

            // Check if this is the loop body (not header/init)
            const Stmt *currentLoop = currentLoopStack.back();
            if (const auto *forStmt = dyn_cast<ForStmt>(currentLoop)) {
                // For loops: body is the CompoundStmt in getBody()
                scope.isLoopBodyScope = (forStmt->getBody() == compoundStmt);
            } else if (const auto *whileStmt = dyn_cast<WhileStmt>(currentLoop)) {
                // While loops: body is the CompoundStmt in getBody()
                scope.isLoopBodyScope = (whileStmt->getBody() == compoundStmt);
            } else if (const auto *doStmt = dyn_cast<DoStmt>(currentLoop)) {
                // Do-while loops: body is the CompoundStmt in getBody()
                scope.isLoopBodyScope = (doStmt->getBody() == compoundStmt);
            } else if (const auto *rangeStmt = dyn_cast<CXXForRangeStmt>(currentLoop)) {
                // Range-based for loops: body is the CompoundStmt in getBody()
                scope.isLoopBodyScope = (rangeStmt->getBody() == compoundStmt);
            }
        }

        // Push scope onto stack
        scopeStack.push_back(scope);

        // ===== AUTOMATIC TRAVERSAL OF CHILDREN =====
        bool result = RecursiveASTVisitor::TraverseCompoundStmt(compoundStmt);

        // ===== POST-TRAVERSAL (after all children have been visited) =====
        // Before popping scope, insert destructor calls
        if (!scopeStack.empty() && !scopeStack.back().variablesInScope.empty()) {
            insertDestructorsForScope(scopeStack.back());
        }

        // Pop scope
        if (!scopeStack.empty()) {
            scopeStack.pop_back();
        }

        REWRITE_LOG() << "  DEBUG: Exiting scope (CompoundStmt)\n";

        return result;
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

                    // If inside a try block, add this variable to that try block's list
                    if (!currentTryBlockStack.empty()) {
                        unsigned tryBlockIndex = currentTryBlockStack.back();
                        tryCatchBlocks[tryBlockIndex].variablesInTryBlock.push_back(varName);
                        REWRITE_LOG() << "    DEBUG: Added variable '" << varName << "' to try block " << tryBlockIndex << "\n";
                    }

                    // Determine initialization form and generate appropriate prefix
                    InitializationForm form = getInitializationForm(varDecl);
                    std::string prefix;

                    // Look up the variable in localVariables to get owning information
                    bool isOwning = false;
                    for (const auto &var : localVariables) {
                        if (var.name == varName) {
                            isOwning = var.isOwning;
                            break;
                        }
                    }

                    switch (form) {
                        case BRACED_INIT:
                            prefix = makeBracedInitPrefix(varName, isOwning);
                            REWRITE_LOG() << "    DEBUG: Using " << (isOwning ? "CO_BRACED_INIT_OWNING" : "CO_BRACED_INIT") << " for variable '" << varName << "'\n";
                            break;
                        case PAREN_INIT:
                            prefix = makeParenInitPrefix(varName, isOwning);
                            REWRITE_LOG() << "    DEBUG: Using " << (isOwning ? "CO_PAREN_INIT_OWNING" : "CO_PAREN_INIT") << " for variable '" << varName << "'\n";
                            break;
                        case CONSTRUCT_CALL:
                        default:
                            prefix = makeParenInitPrefix(varName, isOwning);
                            REWRITE_LOG() << "    DEBUG: Using " << (isOwning ? "CO_PAREN_INIT_OWNING" : "CO_PAREN_INIT") << " for construct call variable '" << varName << "'\n";
                            break;
                    }

                    if (varDecl->hasInit()) {
                        // Get the range for just the declaration part (type + name)
                        SourceLocation declStart = varDecl->getSourceRange().getBegin();
                        SourceLocation initStart = varDecl->getInit()->getSourceRange().getBegin();
                        SourceRange declOnlyRange(declStart, initStart.getLocWithOffset(-1));

                        // Handle different initialization forms
                        if (form == BRACED_INIT) {
                            // For braced initialization, handle ranges properly
                            std::optional<SourceRange> childrenRange = handleBracedInitialization(varDecl);

                            if (childrenRange.has_value()) {
                                REWRITE_LOG() << "range of children is "<< childrenRange.value().printToString(sourceManager) << '\n';
                                // Replace everything from declaration start to first child start with prefix
                                SourceRange prefixRange(declStart, childrenRange->getBegin().getLocWithOffset(-1));
                                declReplacements.emplace_back(prefixRange, prefix, true);

                                // Replace everything from last child end to init end with closing paren
                                SourceLocation initEnd = varDecl->getInit()->getSourceRange().getEnd();
                                SourceLocation afterLastChild = Lexer::getLocForEndOfToken(childrenRange->getEnd(), 0, sourceManager, LangOptions());
                                assert(afterLastChild.isValid() && "Invalid location for last child end");
                                SourceRange suffixRange(afterLastChild, initEnd);
                                declReplacements.emplace_back(suffixRange, ")", true);
                            } else {
                                // Empty braced initialization - replace entire range
                                SourceRange fullRange(declStart, varDecl->getInit()->getSourceRange().getEnd());
                                declReplacements.emplace_back(fullRange, prefix + ")", true);
                            }
                        } else if (form == PAREN_INIT) {
                            // For parenthesized initialization, handle ranges properly
                            std::optional<SourceRange> childrenRange = handleParenthesizedInitialization(varDecl);

                            if (childrenRange.has_value()) {
                                // Replace everything from declaration start to first child start with prefix
                                SourceRange prefixRange(declStart, childrenRange->getBegin().getLocWithOffset(-1));
                                declReplacements.emplace_back(prefixRange, prefix, true);

                                // Process the arguments (now that prefix replacement is set up)
                                processParenthesizedArguments(varDecl);

                                // Find the actual closing parenthesis of the initialization
                                SourceLocation afterLastChild = Lexer::getLocForEndOfToken(childrenRange->getEnd(), 0, sourceManager, LangOptions());
                                
                                // Search for the closing parenthesis after the last argument
                                SourceLocation closingParen = findClosingParen(afterLastChild);
                                if (closingParen.isValid()) {
                                    SourceRange suffixRange(afterLastChild, closingParen);
                                    declReplacements.emplace_back(suffixRange, ")", true);
                                } else {
                                    REWRITE_LOG() << "    WARNING: Could not find closing parenthesis, using init end\n";
                                    SourceLocation initEnd = varDecl->getInit()->getSourceRange().getEnd();
                                    SourceRange suffixRange(afterLastChild, initEnd);
                                    declReplacements.emplace_back(suffixRange, ")", true);
                                }
                            } else {
                                // Empty parenthesized initialization - replace entire range
                                SourceRange fullRange(declStart, varDecl->getInit()->getSourceRange().getEnd());
                                declReplacements.emplace_back(fullRange, prefix + ")", true);
                            }
                        } else {
                            // Replace declaration part with appropriate prefix
                            declReplacements.emplace_back(declOnlyRange, prefix, true);

                            // Regular construct() call - recursively visit the initialization expression
                            TraverseStmt(varDecl->getInit());

                            // Add closing parenthesis at the end
                            SourceLocation initEnd = varDecl->getInit()->getSourceRange().getEnd();
                            SourceLocation afterInit = Lexer::getLocForEndOfToken(initEnd, 0, sourceManager, LangOptions());
                             assert(afterInit.isValid() && "Invalid location for last child end");
                            SourceRange closingRange(afterInit, afterInit);
                            declReplacements.emplace_back(closingRange, ")", false);
                        }
                    } else {
                        // No initialization - replace entire declaration with empty call
                        std::string emptyCall = prefix + ")";
                        SourceRange declRange = varDecl->getSourceRange();
                        declReplacements.emplace_back(declRange, emptyCall, true);
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
        coroStmt.operand = getOriginalCoroutineExprArgument(coyield);
        coroStmt.index = nextCoroStatementIndex++;

        // Find the location of the co_yield keyword
        coroStmt.keywordLoc = coyield->getKeywordLoc();

        // Get the operand range
        if (coroStmt.operand) {
            coroStmt.operandStart = coroStmt.operand->getBeginLoc();
            coroStmt.operandEnd = coroStmt.operand->getEndLoc();

            // Check if the operand expression is a temporary (prvalue)
            // The operand is what comes after co_yield (e.g., the "3" in "co_yield 3")
            if (isPrValue(coroStmt.operand)) {
                std::string operandText = getSourceText(coroStmt.operand, sourceManager);
                std::string operandTypeStr = typeAsString(coroStmt.operand->getType(), *astContext);
                REWRITE_LOG() << "    DEBUG: co_yield operand '" << operandText
                              << "' (type: " << operandTypeStr << ") is a temporary, will use CO_YIELD_BUFFERED\n";

                // Get the actual type passed to yield_value (after implicit conversions)
                // by examining the call to promise.yield_value()
                QualType bufferType = coroStmt.operand->getType(); // Default fallback

                const Expr* yieldCall = coyield->getOperand(); // The call to yield_value
                if (yieldCall) {
                    const Expr* yieldCallStripped = yieldCall->IgnoreImplicit();
                    if (const CallExpr* call = dyn_cast<CallExpr>(yieldCallStripped)) {
                        if (call->getNumArgs() > 0) {
                            const Expr* actualArg = call->getArg(0);
                            QualType convertedType = actualArg->getType();
                            // Remove references to get the decayed type
                            if (convertedType->isReferenceType()) {
                                convertedType = convertedType.getNonReferenceType();
                            }
                            bufferType = convertedType;
                            REWRITE_LOG() << "    DEBUG: Extracted buffer type from yield_value call: "
                                         << typeAsString(bufferType, *astContext) << "\n";
                        }
                    }
                }

                coroutineInfo.yieldedOrAwaitedTemporaries.push_back(bufferType);

                // Mark this statement as needing buffered macro and store the buffer type
                coroStmt.needsBuffering = true;
                coroStmt.bufferType = bufferType;
            } else {
                std::string operandText = getSourceText(coroStmt.operand, sourceManager);
                std::string operandTypeStr = typeAsString(coroStmt.operand->getType(), *astContext);
                REWRITE_LOG() << "    DEBUG: co_yield operand '" << operandText
                              << "' (type: " << operandTypeStr << ") is not a temporary, will use CO_YIELD\n";
                coroStmt.needsBuffering = false;
            }
        }

        // Capture alive variables at this suspension point
        // Traverse scopeStack from innermost to outermost, collecting all variables
        for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
            const ScopeInfo &scope = *it;
            // Add variables from this scope in reverse order (already in reverse from rbegin)
            for (auto varIt = scope.variablesInScope.rbegin(); varIt != scope.variablesInScope.rend(); ++varIt) {
                coroStmt.aliveVariables.push_back(*varIt);
                REWRITE_LOG() << "      DEBUG: Variable '" << *varIt << "' is alive at suspension point " << coroStmt.index << "\n";
            }
        }

        // Collect subexpression temporaries
        if (coroStmt.operand) {
            collectTemporariesFromExpression(coroStmt.operand, coroStmt);

            // Add temporary variable names to alive variables
            for (const auto &temp : coroStmt.temporaries) {
                coroStmt.aliveVariables.push_back(temp.tempVarName);
                REWRITE_LOG() << "      DEBUG: Temporary '" << temp.tempVarName << "' is alive at suspension point " << coroStmt.index << "\n";
            }
        }

        coroutineStatements.push_back(coroStmt);

        REWRITE_LOG() << "    DEBUG: Added co_yield with index " << coroStmt.index << " with " << coroStmt.aliveVariables.size() << " alive variables\n";

        return true;
    }

    // Handle co_await expressions
    bool VisitCoawaitExpr(CoawaitExpr *coawait) {
        REWRITE_LOG() << "  Found co_await expression\n";

        CoroutineStatement coroStmt;
        coroStmt.type = CoroutineStatement::AWAIT;
        coroStmt.stmt = coawait;
        coroStmt.operand = getOriginalCoroutineExprArgument(coawait);
        coroStmt.index = nextCoroStatementIndex++;

        // Find the location of the co_await keyword
        coroStmt.keywordLoc = coawait->getKeywordLoc();

        // Get the operand range
        if (coroStmt.operand) {
            coroStmt.operandStart = coroStmt.operand->getBeginLoc();
            coroStmt.operandEnd = coroStmt.operand->getEndLoc();

            // Check if the operand expression is a temporary (prvalue)
            // The operand is what comes after co_await (e.g., the "someFunc()" in "co_await someFunc()")
            if (isPrValue(coroStmt.operand)) {
                std::string operandText = getSourceText(coroStmt.operand, sourceManager);
                std::string operandTypeStr = typeAsString(coroStmt.operand->getType(), *astContext);
                REWRITE_LOG() << "    DEBUG: co_await operand '" << operandText
                              << "' (type: " << operandTypeStr << ") is a temporary, will use CO_AWAIT_BUFFERED\n";

                // Get the actual type passed to await_transform (after implicit conversions)
                // by examining the call to promise.await_transform() or the awaiter constructor
                QualType bufferType = coroStmt.operand->getType(); // Default fallback

                const Expr* awaitCall = coawait->getOperand(); // The await transformation
                if (awaitCall) {
                    const Expr* awaitCallStripped = awaitCall->IgnoreImplicit();
                    if (const CallExpr* call = dyn_cast<CallExpr>(awaitCallStripped)) {
                        if (call->getNumArgs() > 0) {
                            const Expr* actualArg = call->getArg(0);
                            QualType convertedType = actualArg->getType();
                            // Remove references to get the decayed type
                            if (convertedType->isReferenceType()) {
                                convertedType = convertedType.getNonReferenceType();
                            }
                            bufferType = convertedType;
                            REWRITE_LOG() << "    DEBUG: Extracted buffer type from await call: "
                                         << typeAsString(bufferType, *astContext) << "\n";
                        }
                    }
                }

                coroutineInfo.yieldedOrAwaitedTemporaries.push_back(bufferType);

                // Mark this statement as needing buffered macro and store the buffer type
                coroStmt.needsBuffering = true;
                coroStmt.bufferType = bufferType;
            } else {
                std::string operandText = getSourceText(coroStmt.operand, sourceManager);
                std::string operandTypeStr = typeAsString(coroStmt.operand->getType(), *astContext);
                REWRITE_LOG() << "    DEBUG: co_await operand '" << operandText
                              << "' (type: " << operandTypeStr << ") is not a temporary, will use CO_AWAIT\n";
                coroStmt.needsBuffering = false;
            }
        }

        // Capture alive variables at this suspension point
        // Traverse scopeStack from innermost to outermost, collecting all variables
        for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
            const ScopeInfo &scope = *it;
            // Add variables from this scope in reverse order (already in reverse from rbegin)
            for (auto varIt = scope.variablesInScope.rbegin(); varIt != scope.variablesInScope.rend(); ++varIt) {
                coroStmt.aliveVariables.push_back(*varIt);
                REWRITE_LOG() << "      DEBUG: Variable '" << *varIt << "' is alive at suspension point " << coroStmt.index << "\n";
            }
        }

        // Collect subexpression temporaries
        if (coroStmt.operand) {
            collectTemporariesFromExpression(coroStmt.operand, coroStmt);

            // Add temporary variable names to alive variables
            for (const auto &temp : coroStmt.temporaries) {
                coroStmt.aliveVariables.push_back(temp.tempVarName);
                REWRITE_LOG() << "      DEBUG: Temporary '" << temp.tempVarName << "' is alive at suspension point " << coroStmt.index << "\n";
            }
        }

        coroutineStatements.push_back(coroStmt);

        REWRITE_LOG() << "    DEBUG: Added co_await with index " << coroStmt.index << " with " << coroStmt.aliveVariables.size() << " alive variables\n";

        return true;
    }

    // Handle co_return statements
    bool VisitCoreturnStmt(CoreturnStmt *coreturn) {
        REWRITE_LOG() << "  Found co_return statement\n";

        CoroutineStatement coroStmt;
        coroStmt.type = CoroutineStatement::RETURN;
        coroStmt.stmt = coreturn;
        coroStmt.operand = coreturn->getOperand();  // May be nullptr for co_return;
        coroStmt.index = nextCoroStatementIndex++;

        // Find the location of the co_return keyword
        coroStmt.keywordLoc = coreturn->getKeywordLoc();

        // Get the operand range if present
        if (coroStmt.operand) {
            coroStmt.operandStart = coroStmt.operand->getBeginLoc();
            coroStmt.operandEnd = coroStmt.operand->getEndLoc();
            REWRITE_LOG() << "    DEBUG: co_return has operand\n";
        } else {
            REWRITE_LOG() << "    DEBUG: co_return has no operand (void return)\n";
        }

        // Capture alive variables at this suspension point
        for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
            const ScopeInfo &scope = *it;
            for (auto varIt = scope.variablesInScope.rbegin(); varIt != scope.variablesInScope.rend(); ++varIt) {
                coroStmt.aliveVariables.push_back(*varIt);
                REWRITE_LOG() << "      DEBUG: Variable '" << *varIt << "' is alive at co_return point " << coroStmt.index << "\n";
            }
        }

        coroutineStatements.push_back(coroStmt);

        REWRITE_LOG() << "    DEBUG: Added co_return with index " << coroStmt.index << " with " << coroStmt.aliveVariables.size() << " alive variables\n";

        return true;
    }

    // Handle try-catch blocks - using Traverse for proper pre/post hooks
    bool TraverseCXXTryStmt(CXXTryStmt *tryStmt) {
        // ===== PRE-TRAVERSAL =====
        REWRITE_LOG() << "  Found try-catch block\n";

        TryCatchBlock tryCatch;
        tryCatch.tryStmt = tryStmt;
        tryCatch.index = nextTryCatchIndex++;

        // Get the try keyword location
        tryCatch.tryKeywordLoc = tryStmt->getBeginLoc();

        // Get the try block compound statement
        const CompoundStmt *tryBlock = tryStmt->getTryBlock();
        if (tryBlock) {
            tryCatch.tryBlockStart = tryBlock->getLBracLoc();
            tryCatch.tryBlockEnd = tryBlock->getRBracLoc();
        }

        // Process each catch clause
        unsigned numHandlers = tryStmt->getNumHandlers();
        REWRITE_LOG() << "    DEBUG: Found " << numHandlers << " catch clause(s)\n";

        for (unsigned i = 0; i < numHandlers; ++i) {
            const CXXCatchStmt *catchStmt = tryStmt->getHandler(i);

            // Get the catch clause text
            std::string catchClauseText = processCatchClause(catchStmt);
            tryCatch.catchClauses.push_back(catchClauseText);

            // Update the end location to include all catch clauses
            if (i == numHandlers - 1) {
                tryCatch.catchEnd = catchStmt->getEndLoc();
            }
        }

        // Push this try block index onto the stack before traversing children
        unsigned currentIndex = tryCatch.index;
        currentTryBlockStack.push_back(currentIndex);

        // Store the try-catch block (we'll update variablesInTryBlock later)
        tryCatchBlocks.push_back(tryCatch);

        REWRITE_LOG() << "    DEBUG: Added try-catch block with index " << tryCatch.index << "\n";

        // ===== TRAVERSE CHILDREN =====
        bool result = RecursiveASTVisitor::TraverseCXXTryStmt(tryStmt);

        // ===== POST-TRAVERSAL =====
        // Pop the try block index from the stack
        currentTryBlockStack.pop_back();

        return result;
    }

    // Process a catch clause and transform variable references
    std::string processCatchClause(const CXXCatchStmt *catchStmt) {
        // Get the exception declaration (e.g., "MyException& e" or "...")
        const VarDecl *exceptionDecl = catchStmt->getExceptionDecl();

        std::string catchHeader;
        if (exceptionDecl) {
            // Named exception parameter
            QualType exceptionType = exceptionDecl->getType();
            std::string exceptionTypeName = typeAsString(exceptionType, *astContext);
            std::string exceptionVarName = exceptionDecl->getNameAsString();
            catchHeader = "catch (" + exceptionTypeName + " " + exceptionVarName + ")";
        } else {
            // Catch-all clause
            catchHeader = "catch (...)";
        }

        // Get the catch block body
        const Stmt *handlerBlock = catchStmt->getHandlerBlock();
        if (!handlerBlock) {
            return catchHeader + " {}";
        }

        // Get the source text of the handler block
        SourceRange handlerRange = handlerBlock->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(handlerRange);
        std::string handlerText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();

        // Collect all variable references with their declaration locations using AST
        std::vector<std::pair<SourceRange, SourceLocation>> references;
        VariableReferenceCollector collector(references, variableNames);
        collector.TraverseStmt(const_cast<Stmt*>(handlerBlock));

        // Sort references by position (reverse order so we can replace from end to start)
        std::sort(references.begin(), references.end(),
                  [&](const auto &a, const auto &b) {
                      return sourceManager.getFileOffset(a.first.getBegin()) >
                             sourceManager.getFileOffset(b.first.getBegin());
                  });

        // Replace each reference with CO_GET call using the correct member name
        std::string transformedText = handlerText;
        SourceLocation blockStart = handlerBlock->getBeginLoc();
        unsigned blockStartOffset = sourceManager.getFileOffset(blockStart);

        for (const auto &[refRange, declLoc] : references) {
            // Get the member name from the mapping
            auto it = declLocationToMemberName.find(declLoc);
            if (it == declLocationToMemberName.end()) {
                continue; // Skip if not in mapping
            }
            std::string memberName = it->second;
            std::string replacement = makeStateGetCall(memberName);

            // Calculate positions relative to the block start
            unsigned refStartOffset = sourceManager.getFileOffset(refRange.getBegin());
            unsigned refEndOffset = sourceManager.getFileOffset(refRange.getEnd()) + 1;
            unsigned relativeStart = refStartOffset - blockStartOffset;
            unsigned relativeEnd = refEndOffset - blockStartOffset;

            transformedText.replace(relativeStart, relativeEnd - relativeStart, replacement);
        }

        return catchHeader + " " + transformedText;
    }

    // Transform variable references in arbitrary text
    std::string transformVariableReferencesInText(const std::string &text) {
        std::string result = text;

        // Replace each tracked local variable with CO_GET(varName)
        for (const auto &varName: variableNames) {
            std::string replacement = makeStateGetCall(varName);

            size_t pos = 0;
            while ((pos = result.find(varName, pos)) != std::string::npos) {
                // Check if it's a word boundary
                bool isWordStart = (pos == 0) || !std::isalnum(result[pos - 1]);
                bool isWordEnd = (pos + varName.length() >= result.length()) ||
                                 !std::isalnum(result[pos + varName.length()]);

                if (isWordStart && isWordEnd) {
                    REWRITE_LOG() << "        DEBUG: Replacing '" << varName << "' with '" << replacement <<
                            "' in catch clause at position " << pos << "\n";
                    result.replace(pos, varName.length(), replacement);
                    pos += replacement.length();
                } else {
                    pos += varName.length();
                }
            }
        }

        return result;
    }

    // Handle ranged-for loops - using Traverse for proper pre/post hooks
    bool TraverseCXXForRangeStmt(CXXForRangeStmt *forRange) {
        // ===== PRE-TRAVERSAL (ranged-for processing + loop tracking) =====
        REWRITE_LOG() << "\n=== RANGED-FOR LOOP DETECTION ===\n";
        REWRITE_LOG() << "  Found ranged-for loop at: " << forRange->getSourceRange().getBegin().printToString(
                    sourceManager)
                << " to " << forRange->getSourceRange().getEnd().printToString(sourceManager) << "\n";

        // Add to loop stack for break/continue tracking
        currentLoopStack.push_back(forRange);

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

        // ===== AUTOMATIC TRAVERSAL =====
        bool result = RecursiveASTVisitor::TraverseCXXForRangeStmt(forRange);

        // ===== POST-TRAVERSAL =====
        REWRITE_LOG() << "=== END RANGED-FOR DETECTION ===\n\n";

        // Pop from loop stack
        currentLoopStack.pop_back();

        return result;
    }

    // Handle variable references - but not in declaration contexts
    bool VisitDeclRefExpr(DeclRefExpr *declRef) {
        if (auto *varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
            std::string varName = varDecl->getNameAsString();

            if (variableNames.count(varName)) {
                // Check if this reference is part of a declaration we're already handling
                if (!isPartOfDeclaration(declRef)) {
                    // Get the declaration location to find the actual member name
                    SourceLocation declLoc = varDecl->getLocation();
                    auto it = declLocationToMemberName.find(declLoc);

                    std::string memberName;
                    if (it != declLocationToMemberName.end()) {
                        // Use the mapped member name (which may be renamed for shadowing)
                        memberName = it->second;
                        REWRITE_LOG() << "  Found variable reference: " << varName
                                     << " (member: " << memberName << ")\n";
                    } else {
                        // Fallback: use original name (shouldn't happen after struct generation)
                        memberName = varName;
                        REWRITE_LOG() << "  Found variable reference: " << varName
                                     << " (no mapping found, using original name)\n";
                    }

                    std::string getCall = makeStateGetCall(memberName);
                    SourceRange refRange = declRef->getSourceRange();
                    refReplacements.emplace_back(refRange, getCall, true);
                }
            }
        }
        return true;
    }

    // Handle member expressions - replace with __self->
    bool VisitMemberExpr(MemberExpr *memberExpr) {
        if (!isMemberFunction) {
            return true; // Not a member function, nothing to do
        }

        std::string memberName = memberExpr->getMemberNameInfo().getAsString();
        std::string replacement = "__self->" + memberName;

        // Check if this is an explicit 'this' access
        if (auto *explicitThis = dyn_cast<CXXThisExpr>(memberExpr->getBase())) {
            REWRITE_LOG() << "  Found explicit member access via 'this': " << memberName << "\n";

            // Mark this 'this' expression as processed to avoid double replacement
            processedThisExpressions.insert(explicitThis->getLocation());

            // Replace the entire "this->member" expression with "__self->member"
            SourceRange entireExprRange = memberExpr->getSourceRange();
            refReplacements.emplace_back(entireExprRange, replacement, true);
        } else if (memberExpr->isImplicitAccess()) {
            REWRITE_LOG() << "  Found implicit member access: " << memberName << "\n";

            // Replace the entire member expression with "__self->member"
            SourceRange memberRange = memberExpr->getSourceRange();
            refReplacements.emplace_back(memberRange, replacement, true);
        }

        return true;
    }

    // Handle standalone this expressions (not part of member access)
    bool VisitCXXThisExpr(CXXThisExpr *thisExpr) {
        if (!isMemberFunction) {
            return true; // Not a member function, nothing to do
        }

        // Check if this 'this' expression was already processed by VisitMemberExpr
        SourceLocation thisLoc = thisExpr->getLocation();
        if (processedThisExpressions.count(thisLoc)) {
            REWRITE_LOG() << "  Skipping 'this' expression (already processed by member access)\n";
            return true;
        }

        REWRITE_LOG() << "  Found standalone 'this' expression\n";

        // Replace standalone "this" with "__self"
        SourceRange thisRange = thisExpr->getSourceRange();
        refReplacements.emplace_back(thisRange, "__self", true);

        return true;
    }

    // Track for loops for proper scope handling - using Traverse for pre/post hooks
    bool TraverseForStmt(ForStmt *forStmt) {
        // ===== PRE-TRAVERSAL =====
        REWRITE_LOG() << "  DEBUG: Found for loop\n";
        currentLoopStack.push_back(forStmt);

        // ===== AUTOMATIC TRAVERSAL =====
        bool result = RecursiveASTVisitor::TraverseForStmt(forStmt);

        // ===== POST-TRAVERSAL =====
        currentLoopStack.pop_back();

        return result;
    }

    // Track while loops for proper scope handling - using Traverse for pre/post hooks
    bool TraverseWhileStmt(WhileStmt *whileStmt) {
        // ===== PRE-TRAVERSAL =====
        REWRITE_LOG() << "  DEBUG: Found while loop\n";
        currentLoopStack.push_back(whileStmt);

        // ===== AUTOMATIC TRAVERSAL =====
        bool result = RecursiveASTVisitor::TraverseWhileStmt(whileStmt);

        // ===== POST-TRAVERSAL =====
        currentLoopStack.pop_back();

        return result;
    }

    // Track do-while loops for proper scope handling - using Traverse for pre/post hooks
    bool TraverseDoStmt(DoStmt *doStmt) {
        // ===== PRE-TRAVERSAL =====
        REWRITE_LOG() << "  DEBUG: Found do-while loop\n";
        currentLoopStack.push_back(doStmt);

        // ===== AUTOMATIC TRAVERSAL =====
        bool result = RecursiveASTVisitor::TraverseDoStmt(doStmt);

        // ===== POST-TRAVERSAL =====
        currentLoopStack.pop_back();

        return result;
    }

    // Handle break statements - need to destroy variables in loop scope
    bool VisitBreakStmt(BreakStmt *breakStmt) {
        REWRITE_LOG() << "  Found break statement\n";

        // Find the innermost loop and destroy variables in its scope
        insertLoopVariableDestructors(breakStmt, true /* is break */);

        return true;
    }

    // Handle continue statements - need to destroy variables in loop scope
    bool VisitContinueStmt(ContinueStmt *continueStmt) {
        REWRITE_LOG() << "  Found continue statement\n";

        // Find the innermost loop and destroy variables in its scope
        insertLoopVariableDestructors(continueStmt, false /* is continue */);

        return true;
    }

private:
    enum InitializationForm {
        CONSTRUCT_CALL, // Regular construct() call for other types
        BRACED_INIT,    // Braced initialization like {1, 2}
        PAREN_INIT      // Parenthesized initialization like (1, 2)
    };

    InitializationForm getInitializationForm(const VarDecl *varDecl) {
        if (!varDecl->hasInit()) {
            return CONSTRUCT_CALL;
        }

        const Expr *init = unwrapExpr(varDecl->getInit());

        // Check for braced initialization list
        if (isa<InitListExpr>(init) || isa<CXXStdInitializerListExpr>(init)) {
            REWRITE_LOG() << "    DEBUG: Detected braced initialization\n";
            return BRACED_INIT;
        }

        // Check for constructor call (parenthesized initialization)
        if (isa<CXXConstructExpr>(init)) {
            const auto *constructExpr = cast<CXXConstructExpr>(init);
            if (constructExpr->isListInitialization()) {
                return BRACED_INIT;
            }
            // If it has argumentsoand is direct initialization, it's likely parenthesized
            if (constructExpr->getNumArgs() > 0 && constructExpr->isListInitialization() == false) {
                REWRITE_LOG() << "    DEBUG: Detected parenthesized initialization\n";
                return PAREN_INIT;
            }
        }

        // Check for temporary object creation with braces (like std::vector{1,2})
        if (isa<CXXTemporaryObjectExpr>(init)) {
            const auto *tempExpr = cast<CXXTemporaryObjectExpr>(init);
            if (tempExpr->isListInitialization()) {
                REWRITE_LOG() << "    DEBUG: Detected braced temporary object initialization\n";
                return BRACED_INIT;
            } else {
                REWRITE_LOG() << "    DEBUG: Detected parenthesized temporary object initialization\n";
                return PAREN_INIT;
            }
        }

        REWRITE_LOG() << "    DEBUG: Using default construct() call\n";
        return CONSTRUCT_CALL;
    }

    std::optional<SourceRange> handleParenthesizedInitialization(const VarDecl *varDecl) {
        if (!varDecl->hasInit()) {
            return std::nullopt;
        }

        const Expr *init = unwrapExpr(varDecl->getInit());
        std::vector<const Expr*> arguments;

        // Extract arguments from parenthesized initialization
        if (const auto *constructExpr = dyn_cast<CXXConstructExpr>(init)) {
            // Constructor call like std::vector(1, 2)
            for (unsigned i = 0; i < constructExpr->getNumArgs(); ++i) {
                arguments.push_back(constructExpr->getArg(i));
            }
        } else if (const auto *tempExpr = dyn_cast<CXXTemporaryObjectExpr>(init)) {
            // Temporary object with parentheses like std::vector(1, 2)
            for (unsigned i = 0; i < tempExpr->getNumArgs(); ++i) {
                arguments.push_back(tempExpr->getArg(i));
            }
        }

        if (arguments.empty()) {
            REWRITE_LOG() << "    DEBUG: No arguments found in parenthesized initialization\n";
            return std::nullopt;
        }

        // Find first and last arguments with valid source locations
        SourceLocation firstValidStart;
        SourceLocation lastValidEnd;
        bool foundFirst = false;
        bool foundLast = false;

        // Find first argument with valid source location
        for (const auto *arg : arguments) {
            SourceRange argRange = arg->getSourceRange();
            if (argRange.getBegin().isValid() && argRange.getEnd().isValid()) {
                firstValidStart = argRange.getBegin();
                foundFirst = true;
                break;
            }
        }

        // Find last argument with valid source location (search backwards)
        for (auto it = arguments.rbegin(); it != arguments.rend(); ++it) {
            SourceRange argRange = (*it)->getSourceRange();
            if (argRange.getBegin().isValid() && argRange.getEnd().isValid()) {
                lastValidEnd = argRange.getEnd();
                foundLast = true;
                break;
            }
        }

        // If no arguments have valid source locations, return nullopt
        if (!foundFirst || !foundLast) {
            REWRITE_LOG() << "    DEBUG: No arguments with valid source locations found in parenthesized initialization\n";
            return std::nullopt;
        }

        REWRITE_LOG() << "    DEBUG: Found " << arguments.size() << " parenthesized initialization arguments\n";

        return SourceRange(firstValidStart, lastValidEnd);
    }

    // Helper function to find the closing parenthesis starting from a given location
    SourceLocation findClosingParen(SourceLocation startLoc) {
        if (!startLoc.isValid()) {
            return SourceLocation();
        }

        // Get the source buffer
        bool invalid = false;
        const char *bufferStart = sourceManager.getCharacterData(startLoc, &invalid);
        if (invalid) {
            return SourceLocation();
        }

        // Search for the closing parenthesis, skipping nested parentheses
        const char *current = bufferStart;
        int parenDepth = 0;
        
        while (*current != '\0') {
            if (*current == '(') {
                parenDepth++;
            } else if (*current == ')') {
                if (parenDepth == 0) {
                    // Found the matching closing parenthesis
                    return startLoc.getLocWithOffset(current - bufferStart);
                }
                parenDepth--;
            }
            current++;
        }

        // Didn't find a matching closing parenthesis
        return SourceLocation();
    }

    void processParenthesizedArguments(const VarDecl *varDecl) {
        if (!varDecl->hasInit()) {
            return;
        }

        const Expr *init = varDecl->getInit();

        // Process arguments from parenthesized initialization
        if (const auto *constructExpr = dyn_cast<CXXConstructExpr>(init)) {
            // Constructor call like std::vector(1, 2)
            for (unsigned i = 0; i < constructExpr->getNumArgs(); ++i) {
                TraverseStmt(const_cast<Stmt*>(reinterpret_cast<const Stmt*>(constructExpr->getArg(i))));
            }
        } else if (const auto *tempExpr = dyn_cast<CXXTemporaryObjectExpr>(init)) {
            // Temporary object with parentheses like std::vector(1, 2)
            for (unsigned i = 0; i < tempExpr->getNumArgs(); ++i) {
                TraverseStmt(const_cast<Stmt*>(reinterpret_cast<const Stmt*>(tempExpr->getArg(i))));
            }
        }

        REWRITE_LOG() << "    DEBUG: Processed parenthesized initialization arguments\n";
    }

    void insertLoopVariableDestructors(const Stmt *stmt, bool isBreak) {
        if (currentLoopStack.empty()) {
            REWRITE_LOG() << "    DEBUG: No loop context found for " << (isBreak ? "break" : "continue") << "\n";
            return;
        }

        // Find all variables that need to be destroyed for break/continue
        std::vector<std::string> varsToDestroy;
        const Stmt *innerMostLoop = currentLoopStack.back();

        // Traverse scope stack from innermost to outermost
        for (auto scopeIt = scopeStack.rbegin(); scopeIt != scopeStack.rend(); ++scopeIt) {
            const auto &scope = *scopeIt;

            // Stop when we exit the current loop's scopes
            if (scope.isLoopScope && scope.loopStmt != innerMostLoop) {
                break;
            }

            // Both break and continue: only destroy variables from loop body scopes, not header/init scopes
            bool shouldDestroyFromThisScope = scope.isLoopScope && scope.isLoopBodyScope && scope.loopStmt == innerMostLoop;

            if (shouldDestroyFromThisScope) {
                for (const auto &varName : scope.variablesInScope) {
                    // Only destroy variables that are tracked (local variables)
                    if (variableNames.count(varName)) {
                        varsToDestroy.push_back(varName);
                        REWRITE_LOG() << "    DEBUG: Will destroy variable '" << varName
                                      << "' from " << (scope.isLoopBodyScope ? "body" : "header") << " scope\n";
                    }
                }
            }
        }

        if (!varsToDestroy.empty()) {
            REWRITE_LOG() << "    DEBUG: Need to destroy " << varsToDestroy.size()
                          << " variables for " << (isBreak ? "break" : "continue") << "\n";

            // Insert destructor calls before the break/continue statement
            SourceLocation insertLoc = stmt->getBeginLoc();
            std::string destructorCalls;

            // Generate destroy calls in reverse order (LIFO) - variables are already in reverse order from rbegin()
            for (const auto &varName : varsToDestroy) {
                destructorCalls += makeStateDestroyCall(varName) + ";\n    ";
            }

            if (!destructorCalls.empty()) {
                SourceRange insertRange(insertLoc, insertLoc);
                refReplacements.emplace_back(insertRange, destructorCalls, false);
            }
        } else {
            REWRITE_LOG() << "    DEBUG: No variables to destroy for " << (isBreak ? "break" : "continue") << "\n";
        }
    }

    std::optional<SourceRange> handleBracedInitialization(const VarDecl *varDecl) {
        if (!varDecl->hasInit()) {
            return std::nullopt;
        }

        const Expr *init = unwrapExpr(varDecl->getInit());

        if (const auto *initList = dyn_cast<InitListExpr>(init)) {
            REWRITE_LOG() << "    DEBUG: Processing InitListExpr with " << initList->getNumInits() << " elements\n";

            if (initList->getNumInits() == 0) {
                // Empty initialization list - no children range
                return std::nullopt;
            }

            // Find first and last arguments with valid source locations
            SourceLocation firstValidStart;
            SourceLocation lastValidEnd;
            bool foundFirst = false;
            bool foundLast = false;

            // Find first argument with valid source location
            for (unsigned i = 0; i < initList->getNumInits(); ++i) {
                const Expr *arg = initList->getInit(i);
                SourceRange argRange = arg->getSourceRange();
                if (argRange.getBegin().isValid() && argRange.getEnd().isValid()) {
                    firstValidStart = argRange.getBegin();
                    foundFirst = true;
                    break;
                }
            }

            // Find last argument with valid source location (search backwards)
            for (int i = initList->getNumInits() - 1; i >= 0; --i) {
                const Expr *arg = initList->getInit(i);
                SourceRange argRange = arg->getSourceRange();
                if (argRange.getBegin().isValid() && argRange.getEnd().isValid()) {
                    lastValidEnd = argRange.getEnd();
                    foundLast = true;
                    break;
                }
            }

            // If no arguments have valid source locations, return nullopt
            if (!foundFirst || !foundLast) {
                REWRITE_LOG() << "    DEBUG: No arguments with valid source locations found in InitListExpr\n";
                return std::nullopt;
            }

            SourceRange childrenRange(firstValidStart, lastValidEnd);

            // Process each initialization element individually
            for (unsigned i = 0; i < initList->getNumInits(); ++i) {
                TraverseStmt(const_cast<Stmt*>(reinterpret_cast<const Stmt*>(initList->getInit(i))));
            }

            return childrenRange;

        } else if (const auto *constructExpr = dyn_cast<CXXConstructExpr>(init)) {
            // Handle CXXConstructExpr with list initialization
            if (constructExpr->isListInitialization() && constructExpr->getNumArgs() > 0) {
                REWRITE_LOG() << "    DEBUG: Processing CXXConstructExpr list initialization with " << constructExpr->getNumArgs() << " args\n";

                // Find first and last arguments with valid source locations
                SourceLocation firstValidStart;
                SourceLocation lastValidEnd;
                bool foundFirst = false;
                bool foundLast = false;

                // Find first argument with valid source location
                for (unsigned i = 0; i < constructExpr->getNumArgs(); ++i) {
                    const Expr *arg = constructExpr->getArg(i);
                    SourceRange argRange = arg->getSourceRange();
                    if (argRange.getBegin().isValid() && argRange.getEnd().isValid()) {
                        // Skip the `{` before.
                        firstValidStart = argRange.getBegin().getLocWithOffset(1);
                        foundFirst = true;
                        break;
                    }
                }

                // Find last argument with valid source location (search backwards)
                for (int i = constructExpr->getNumArgs() - 1; i >= 0; --i) {
                    const Expr *arg = constructExpr->getArg(i);
                    SourceRange argRange = arg->getSourceRange();
                    if (argRange.getBegin().isValid() && argRange.getEnd().isValid()) {
                        // Skip the `}` after.
                        lastValidEnd = argRange.getEnd().getLocWithOffset(-1);
                        foundLast = true;
                        break;
                    }
                }

                // If no arguments have valid source locations, return nullopt
                if (!foundFirst || !foundLast) {
                    REWRITE_LOG() << "    DEBUG: No arguments with valid source locations found in CXXConstructExpr\n";
                    return std::nullopt;
                }

                SourceRange argsRange(firstValidStart, lastValidEnd);

                // Process arguments individually
                for (unsigned i = 0; i < constructExpr->getNumArgs(); ++i) {
                    TraverseStmt(const_cast<Stmt*>(reinterpret_cast<const Stmt*>(constructExpr->getArg(i))));
                }

                return argsRange;
            }
        }

        return std::nullopt;
    }

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

    // Collect all MaterializeTemporaryExpr nodes from an expression and create TemporaryInfo
    void collectTemporariesFromExpression(const Expr *expr, CoroutineStatement &coroStmt) {
        if (!expr) return;

        std::vector<const MaterializeTemporaryExpr*> matTemps;
        TemporaryCollector collector(matTemps);
        collector.TraverseStmt(const_cast<Expr*>(expr));

        // Get the operand's begin location to filter out top-level temporaries
        SourceLocation operandBegin = expr->getBeginLoc();

        REWRITE_LOG() << "      DEBUG: Found " << matTemps.size() << " temporaries in expression\n";
        REWRITE_LOG() << "      DEBUG: Operand begins at " << operandBegin.printToString(sourceManager) << "\n";

        unsigned tempIndex = 0;
        for (const auto *matTemp : matTemps) {
            // Skip temporaries that start at the same location as the operand
            // These are likely wrapping the entire operand (e.g., implicit object of member calls)
            // We only want subexpression temporaries, not top-level structural temporaries
            if (matTemp->getBeginLoc() == operandBegin) {
                REWRITE_LOG() << "      DEBUG: Skipping temporary at operand begin location (wraps entire operand)\n";
                continue;
            }

            TemporaryInfo tempInfo;
            tempInfo.expr = matTemp;
            tempInfo.tempVarName = "temp_" + std::to_string(coroStmt.index) + "_" + std::to_string(tempIndex++);

            // Get the subexpression being materialized
            const Expr *subExpr = matTemp->getSubExpr();
            tempInfo.type = matTemp->getType();
            tempInfo.constructLoc = matTemp->getBeginLoc();

            // Determine initialization style and extract arguments
            // Check if it's a braced-init-list or constructor call
            if (auto *initListExpr = dyn_cast<InitListExpr>(subExpr)) {
                tempInfo.isBracedInit = true;
                tempInfo.initArgs = handleInitListArgs(initListExpr);
                REWRITE_LOG() << "        DEBUG: Temporary " << tempInfo.tempVarName
                             << " uses braced init: {" << tempInfo.initArgs << "}\n";
            } else if (auto *constructExpr = dyn_cast<CXXConstructExpr>(subExpr)) {
                tempInfo.isBracedInit = false;
                tempInfo.initArgs = handleConstructorArgs(constructExpr);
                REWRITE_LOG() << "        DEBUG: Temporary " << tempInfo.tempVarName
                             << " uses paren init: (" << tempInfo.initArgs << ")\n";
            } else {
                // Fallback: use the entire subexpression as init arg
                tempInfo.isBracedInit = false;
                tempInfo.initArgs = rewriteExpressionExceptVar(subExpr, "");
                REWRITE_LOG() << "        DEBUG: Temporary " << tempInfo.tempVarName
                             << " uses expression init: " << tempInfo.initArgs << "\n";
            }

            coroStmt.temporaries.push_back(tempInfo);
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

        // Collect all variable references with their declaration locations
        std::vector<std::pair<SourceRange, SourceLocation>> references;
        VariableReferenceCollector collector(references, variableNames);
        collector.TraverseStmt(const_cast<Expr*>(expr));

        // Sort references by position (reverse order so we can replace from end to start)
        std::sort(references.begin(), references.end(),
                  [&](const auto &a, const auto &b) {
                      return sourceManager.getFileOffset(a.first.getBegin()) >
                             sourceManager.getFileOffset(b.first.getBegin());
                  });

        // Replace each reference with CO_GET call using the correct member name
        std::string result = exprText;
        SourceLocation exprStart = expr->getBeginLoc();
        unsigned exprStartOffset = sourceManager.getFileOffset(exprStart);

        for (const auto &[refRange, declLoc] : references) {
            // Get the member name from the mapping
            auto it = declLocationToMemberName.find(declLoc);
            if (it == declLocationToMemberName.end()) {
                continue; // Skip if not in mapping
            }
            std::string memberName = it->second;
            std::string replacement = makeStateGetCall(memberName);

            // Calculate positions relative to the expression start
            unsigned refStartOffset = sourceManager.getFileOffset(refRange.getBegin());
            unsigned refEndOffset = sourceManager.getFileOffset(refRange.getEnd()) + 1;
            unsigned relativeStart = refStartOffset - exprStartOffset;
            unsigned relativeEnd = refEndOffset - exprStartOffset;

            REWRITE_LOG() << "        DEBUG: Replacing variable reference with '" << replacement <<
                    "' at offset " << relativeStart << "-" << relativeEnd << "\n";

            result.replace(relativeStart, relativeEnd - relativeStart, replacement);
        }

        if (result != exprText) {
            REWRITE_LOG() << "        DEBUG: rewriteExpression output: '" << result << "'\n";
        } else {
            REWRITE_LOG() << "        DEBUG: No changes made to expression\n";
        }

        return result;
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

    // Helper function to collect replacements for co_yield or co_await (they work identically)
    void collectYieldOrAwaitReplacement(const CoroutineStatement &coroStmt, const std::string &macroBaseName) {
        std::string macroName = coroStmt.needsBuffering ? macroBaseName + "_BUFFERED" : macroBaseName;
        REWRITE_LOG() << "    DEBUG: Collecting " << macroBaseName << " replacement for " << macroName << "(" << coroStmt.index <<
                ", ...)\n";

        // Priority based on index for consistent ordering
        int priority = static_cast<int>(coroStmt.index);

        // Replace keyword with appropriate macro
        // For buffered macros, include the type in parentheses as the first argument
        std::string macroStart;
        if (coroStmt.needsBuffering) {
            std::string bufferTypeStr = typeAsString(coroStmt.bufferType, *astContext);
            macroStart = macroName + "((" + bufferTypeStr + "), " + std::to_string(coroStmt.index) + ", ";
            REWRITE_LOG() << "      DEBUG: Buffered macro with type: " << macroStart << "\n";
        } else {
            macroStart = macroName + "(" + std::to_string(coroStmt.index) + ", ";
        }

        // Replace just the keyword
        SourceLocation keywordEnd = Lexer::getLocForEndOfToken(
            coroStmt.keywordLoc, 0, sourceManager, LangOptions());
        SourceRange keywordRange(coroStmt.keywordLoc, keywordEnd);
        globalReplacements.emplace_back(keywordRange, macroStart, priority, true);

        // Process subexpression temporaries - use incremental replacements for recursive processing
        // Process in reverse order (innermost first) by assigning lower priorities to later temps
        for (size_t tempIdx = 0; tempIdx < coroStmt.temporaries.size(); ++tempIdx) {
            const auto &temp = coroStmt.temporaries[tempIdx];
            REWRITE_LOG() << "      DEBUG: Processing temporary '" << temp.tempVarName << "' (index " << tempIdx << ")\n";

            std::string initMacroName = temp.isBracedInit ? "CO_BRACED_INIT_OWNING" : "CO_PAREN_INIT_OWNING";
            std::string macroOpening = initMacroName + "(" + temp.tempVarName + ", ";

            // Each temporary gets its own priority range to avoid conflicts
            // Innermost temps (later in the vector) get lower priorities so they're processed first
            int tempPriorityBase = priority + 100 + (int)(coroStmt.temporaries.size() - tempIdx - 1) * 10;

            // Get the subexpression from the MaterializeTemporaryExpr
            const Expr *tempSubExpr = temp.expr->getSubExpr();

            if (temp.isBracedInit) {
                // For braced initialization, find the InitListExpr and its braces
                const InitListExpr *initList = nullptr;

                // The subExpr might be a CXXConstructExpr wrapping an InitListExpr
                if (auto *constructExpr = dyn_cast<CXXConstructExpr>(tempSubExpr)) {
                    for (unsigned i = 0; i < constructExpr->getNumArgs(); ++i) {
                        if (auto *ilist = dyn_cast<InitListExpr>(constructExpr->getArg(i)->IgnoreImplicit())) {
                            initList = ilist;
                            break;
                        }
                    }
                } else if (auto *ilist = dyn_cast<InitListExpr>(tempSubExpr)) {
                    initList = ilist;
                }

                if (initList && initList->getLBraceLoc().isValid() && initList->getRBraceLoc().isValid()) {
                    // Insert macro opening before the entire temporary expression
                    SourceLocation tempStart = tempSubExpr->getBeginLoc();
                    SourceRange macroStartRange(tempStart, tempStart);
                    globalReplacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                    // Delete the opening brace
                    SourceLocation lbrace = initList->getLBraceLoc();
                    SourceLocation afterLBrace = lbrace.getLocWithOffset(1);
                    SourceRange lbraceRange(lbrace, afterLBrace);
                    globalReplacements.emplace_back(lbraceRange, "", tempPriorityBase + 1, true);

                    // Delete the closing brace and insert closing paren
                    SourceLocation rbrace = initList->getRBraceLoc();
                    SourceLocation afterRBrace = rbrace.getLocWithOffset(1);
                    SourceRange rbraceRange(rbrace, afterRBrace);
                    globalReplacements.emplace_back(rbraceRange, ")", tempPriorityBase + 2, true);

                    REWRITE_LOG() << "        DEBUG: Will use incremental replacements for braced init temp (priority " << tempPriorityBase << ")\n";
                } else {
                    // Fallback: wrap the entire expression
                    SourceLocation tempStart = tempSubExpr->getBeginLoc();
                    SourceLocation tempEnd = Lexer::getLocForEndOfToken(
                        tempSubExpr->getEndLoc(), 0, sourceManager, LangOptions());

                    SourceRange macroStartRange(tempStart, tempStart);
                    globalReplacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                    SourceRange macroEndRange(tempEnd, tempEnd);
                    globalReplacements.emplace_back(macroEndRange, ")", tempPriorityBase + 2, false);

                    REWRITE_LOG() << "        DEBUG: Will wrap braced init temp (fallback, priority " << tempPriorityBase << ")\n";
                }
            } else {
                // For paren expressions or general expressions
                // Check if it's a ParenExpr and strip the parens
                if (auto *parenExpr = dyn_cast<ParenExpr>(tempSubExpr)) {
                    // It's a parenthesized expression - strip the parens
                    SourceLocation lparenLoc = parenExpr->getLParen();
                    SourceLocation rparenLoc = parenExpr->getRParen();

                    if (lparenLoc.isValid() && rparenLoc.isValid()) {
                        // Insert macro opening before the opening paren
                        SourceRange macroStartRange(lparenLoc, lparenLoc);
                        globalReplacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                        // Delete the opening paren
                        SourceLocation afterLParen = lparenLoc.getLocWithOffset(1);
                        SourceRange lparenRange(lparenLoc, afterLParen);
                        globalReplacements.emplace_back(lparenRange, "", tempPriorityBase + 1, true);

                        // Delete the closing paren and insert closing paren for macro
                        SourceLocation afterRParen = Lexer::getLocForEndOfToken(rparenLoc, 0, sourceManager, LangOptions());
                        SourceRange rparenRange(rparenLoc, afterRParen);
                        globalReplacements.emplace_back(rparenRange, ")", tempPriorityBase + 2, true);

                        REWRITE_LOG() << "        DEBUG: Will strip parens and use CO_PAREN_INIT_OWNING for ParenExpr (priority " << tempPriorityBase << ")\n";
                    } else {
                        // Fallback: just wrap
                        SourceLocation tempStart = tempSubExpr->getBeginLoc();
                        SourceLocation tempEnd = Lexer::getLocForEndOfToken(
                            tempSubExpr->getEndLoc(), 0, sourceManager, LangOptions());

                        SourceRange macroStartRange(tempStart, tempStart);
                        globalReplacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                        SourceRange macroEndRange(tempEnd, tempEnd);
                        globalReplacements.emplace_back(macroEndRange, ")", tempPriorityBase + 2, false);

                        REWRITE_LOG() << "        DEBUG: Will wrap ParenExpr (fallback, priority " << tempPriorityBase << ")\n";
                    }
                } else {
                    // Not a paren expression - just wrap
                    SourceLocation tempStart = tempSubExpr->getBeginLoc();
                    SourceLocation tempEnd = Lexer::getLocForEndOfToken(
                        tempSubExpr->getEndLoc(), 0, sourceManager, LangOptions());

                    SourceRange macroStartRange(tempStart, tempStart);
                    globalReplacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                    SourceRange macroEndRange(tempEnd, tempEnd);
                    globalReplacements.emplace_back(macroEndRange, ")", tempPriorityBase + 2, false);

                    REWRITE_LOG() << "        DEBUG: Will wrap general expression temp (priority " << tempPriorityBase << ")\n";
                }
            }
        }

        // Insert closing parenthesis after the operand (before the semicolon)
        if (coroStmt.operand) {
            SourceLocation operandEnd = Lexer::getLocForEndOfToken(
                coroStmt.operandEnd, 0, sourceManager, LangOptions());

            // Insert closing paren before the semicolon (right after the operand)
            SourceRange parenRange(operandEnd, operandEnd);
            globalReplacements.emplace_back(parenRange, ")", priority + 1000, false);
        } else {
            // No operand case - just insert closing paren right after the macro start
            SourceRange parenRange(keywordEnd, keywordEnd);
            globalReplacements.emplace_back(parenRange, ")", priority + 1000, false);
        }

        // Find the location to insert destructor calls (after the semicolon)
        // Strategy: start from statement end, skip any semicolons, and insert before the first non-semicolon token
        SourceLocation stmtEnd = coroStmt.stmt->getEndLoc();
        SourceLocation searchStart = Lexer::getLocForEndOfToken(
            stmtEnd, 0, sourceManager, LangOptions());

        REWRITE_LOG() << "      DEBUG: Statement end location: " << stmtEnd.printToString(sourceManager) << "\n";

        // Find tokens after the statement, skipping semicolons
        SourceLocation insertionPointAfterSemicolon = searchStart;
        while (true) {
            std::optional<Token> nextTokOpt = Lexer::findNextToken(searchStart, sourceManager, LangOptions());
            if (!nextTokOpt.has_value()) {
                // No more tokens, use current position
                REWRITE_LOG() << "      DEBUG: No more tokens found\n";
                break;
            }

            if (nextTokOpt->is(tok::semi)) {
                // Found a semicolon, move past it and continue searching
                searchStart = Lexer::getLocForEndOfToken(
                    nextTokOpt->getLocation(), 0, sourceManager, LangOptions());
                insertionPointAfterSemicolon = searchStart;
                REWRITE_LOG() << "      DEBUG: Found semicolon at " << nextTokOpt->getLocation().printToString(sourceManager)
                             << ", will insert after it\n";
            } else {
                // Found a non-semicolon token, insert before it
                insertionPointAfterSemicolon = nextTokOpt->getLocation();
                REWRITE_LOG() << "      DEBUG: Found non-semicolon token at " << nextTokOpt->getLocation().printToString(sourceManager)
                             << ", will insert before it\n";
                break;
            }
        }

        REWRITE_LOG() << "      DEBUG: Final insertion point: " << insertionPointAfterSemicolon.printToString(sourceManager) << "\n";

        // Add destruction calls for temporaries after the semicolon
        if (!coroStmt.temporaries.empty()) {
            REWRITE_LOG() << "      DEBUG: Adding destruction calls for " << coroStmt.temporaries.size() << " temporaries\n";

            std::string destructorCalls = "\n";
            // Destroy in reverse order (last created first destroyed)
            for (auto it = coroStmt.temporaries.rbegin(); it != coroStmt.temporaries.rend(); ++it) {
                destructorCalls += "        this->state." + it->tempVarName + ".destroy();\n";
            }

            // Insert after the semicolon
            SourceRange afterSemiRange(insertionPointAfterSemicolon, insertionPointAfterSemicolon);
            globalReplacements.emplace_back(afterSemiRange, destructorCalls, priority + 2000, false);
        }
    }

    void collectCoroutineStatementReplacements() {
        REWRITE_LOG() << "  DEBUG: Collecting coroutine statement replacements for " << coroutineStatements.size() <<
                " statements\n";

        for (const auto &coroStmt: coroutineStatements) {
            if (coroStmt.type == CoroutineStatement::YIELD) {
                collectYieldOrAwaitReplacement(coroStmt, "CO_YIELD");
            } else if (coroStmt.type == CoroutineStatement::AWAIT) {
                collectYieldOrAwaitReplacement(coroStmt, "CO_AWAIT");
            } else if (coroStmt.type == CoroutineStatement::RETURN) {
                REWRITE_LOG() << "    DEBUG: Collecting co_return replacement for index " << coroStmt.index << "\n";

                int priority = static_cast<int>(coroStmt.index);
                SourceLocation keywordEnd = Lexer::getLocForEndOfToken(
                    coroStmt.keywordLoc, 0, sourceManager, LangOptions());

                if (!coroStmt.operand) {
                    // Case 1: co_return; (no operand) -> CO_RETURN_VOID(index);
                    REWRITE_LOG() << "      DEBUG: co_return has no operand, using CO_RETURN_VOID\n";

                    std::string replacement = "CO_RETURN_VOID(" + std::to_string(coroStmt.index) + ");";
                    SourceRange keywordRange(coroStmt.keywordLoc, keywordEnd);
                    globalReplacements.emplace_back(keywordRange, replacement, priority, true);
                } else {
                    // Check if operand type is void
                    QualType operandType = coroStmt.operand->getType();
                    bool isVoidType = operandType->isVoidType();

                    if (isVoidType) {
                        // Case 2: co_return expr; where expr is void -> expr; CO_RETURN_VOID(index);
                        REWRITE_LOG() << "      DEBUG: co_return operand is void type, using CO_RETURN_VOID\n";

                        // Replace "co_return " with nothing (just remove the keyword and space)
                        SourceRange keywordRange(coroStmt.keywordLoc, keywordEnd);
                        globalReplacements.emplace_back(keywordRange, "", priority, true);

                        // Insert "; CO_RETURN_VOID(index)" after the operand
                        SourceLocation operandEnd = Lexer::getLocForEndOfToken(
                            coroStmt.operandEnd, 0, sourceManager, LangOptions());
                        SourceRange insertRange(operandEnd, operandEnd);
                        std::string insertion = "; CO_RETURN_VOID(" + std::to_string(coroStmt.index) + ")";
                        globalReplacements.emplace_back(insertRange, insertion, priority + 1000, false);
                    } else {
                        // Case 3: co_return expr; where expr is non-void -> CO_RETURN_VALUE(index, (expr))
                        REWRITE_LOG() << "      DEBUG: co_return operand is value type, using CO_RETURN_VALUE\n";

                        // Replace "co_return" with "CO_RETURN_VALUE(index, ("
                        std::string macroStart = "CO_RETURN_VALUE(" + std::to_string(coroStmt.index) + ", (";
                        SourceRange keywordRange(coroStmt.keywordLoc, keywordEnd);
                        globalReplacements.emplace_back(keywordRange, macroStart, priority, true);

                        // Insert "))" after the operand
                        SourceLocation operandEnd = Lexer::getLocForEndOfToken(
                            coroStmt.operandEnd, 0, sourceManager, LangOptions());
                        SourceRange parenRange(operandEnd, operandEnd);
                        globalReplacements.emplace_back(parenRange, "))", priority + 1000, false);
                    }
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
            // Use rewriteExpression to handle variable references in the range expression
            std::string rangeExprRewritten = rewriteExpression(rangedFor.rangeExpr);
            std::string constructCalls =
                    makeStateConstructCall(rangedFor.rangeVarName, rangeExprRewritten) + ";\n" +
                    "    " + makeStateConstructCall(rangedFor.beginVarName, makeStateGetCall(
                                                                                rangedFor.rangeVarName) + ".begin()") +
                    ";\n" +
                    "    " + makeStateConstructCall(rangedFor.endVarName, makeStateGetCall(
                                                                              rangedFor.rangeVarName) + ".end()") +
                    ";\n" +
                    "    for (; " + makeStateGetCall(rangedFor.beginVarName) + " != " + makeStateGetCall(
                        rangedFor.endVarName) +
                    "; ++" + makeStateGetCall(rangedFor.beginVarName) + ")";

            int headerPriority = 10000 + static_cast<int>(rangedFor.index);
            globalReplacements.emplace_back(headerRange, constructCalls, headerPriority, true);

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
                globalReplacements.emplace_back(insertRange, constructCall, constructPriority, true);

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
                if (!std::exchange(insertedBrace, true)) { concatenatedReplacement += "}"; }
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

            insertBrace(); // Re-add the brace after the concatenated replacement.
            // We deliberately remove and reinsert the brace.
            globalReplacements.emplace_back(braceRange, concatenatedReplacement, minPriority, true);

            REWRITE_LOG() << "    Added concatenated replacement (" << concatenatedReplacement.length() << " chars) at "
                    << braceLocation.printToString(sourceManager) << " with priority " << minPriority << "\n";
        }

        REWRITE_LOG() << "  Processed " << scopeEndReplacements.size() << " scope end positions\n";
    }

public:
    void collectTryCatchReplacements() {
        REWRITE_LOG() << "  DEBUG: Collecting try-catch replacements for " << tryCatchBlocks.size() << " blocks\n";

        for (const auto &tryCatch: tryCatchBlocks) {
            REWRITE_LOG() << "\n=== COLLECTING TRY-CATCH REPLACEMENTS ===\n";
            REWRITE_LOG() << "    DEBUG: Processing try-catch block " << tryCatch.index << "\n";

            // Part 1: Replace "try" keyword with "TRY_BEGIN(index)"
            if (tryCatch.tryKeywordLoc.isValid()) {
                SourceLocation tryEnd = Lexer::getLocForEndOfToken(
                    tryCatch.tryKeywordLoc, 0, sourceManager, LangOptions());
                SourceRange tryKeywordRange(tryCatch.tryKeywordLoc, tryEnd);

                std::string tryBegin = "TRY_BEGIN(" + std::to_string(std::numeric_limits<size_t>::max() - tryCatch.index) + "ULL);";
                int tryPriority = 20000 + static_cast<int>(tryCatch.index);
                globalReplacements.emplace_back(tryKeywordRange, tryBegin, tryPriority, true);

                REWRITE_LOG() << "        Added try keyword replacement with TRY_BEGIN(" << tryCatch.index << ") at "
                        << tryKeywordRange.printToString(sourceManager) << " (priority " << tryPriority << ")\n";
            }

            // Part 2: Insert "TRY_END(index)" after the closing brace of try block
            if (tryCatch.tryBlockEnd.isValid()) {
                SourceLocation afterBrace = Lexer::getLocForEndOfToken(
                    tryCatch.tryBlockEnd, 0, sourceManager, LangOptions());
                SourceRange endRange(afterBrace, afterBrace);

                std::string tryEnd = " TRY_END(" + std::to_string(std::numeric_limits<size_t>::max() - tryCatch.index) + "ULL);";
                int endPriority = 30000 + static_cast<int>(tryCatch.index) + 1;
                globalReplacements.emplace_back(endRange, tryEnd, endPriority, false);

                REWRITE_LOG() << "        Added TRY_END(" << tryCatch.index << ") insertion at "
                        << endRange.printToString(sourceManager) << " (priority " << endPriority << ")\n";
            }

            // Part 3: Delete all catch clauses (from after try block to end of catch clauses)
            if (tryCatch.tryBlockEnd.isValid() && tryCatch.catchEnd.isValid()) {
                SourceLocation catchStart = Lexer::getLocForEndOfToken(
                    tryCatch.tryBlockEnd, 0, sourceManager, LangOptions());
                SourceLocation catchEndAfter = Lexer::getLocForEndOfToken(
                    tryCatch.catchEnd, 0, sourceManager, LangOptions());

                SourceRange catchRange(catchStart, catchEndAfter);

                // Replace all catch clauses with empty string
                int deletePriority = 20000 + static_cast<int>(tryCatch.index) + 2;
                globalReplacements.emplace_back(catchRange, "", deletePriority, true);

                REWRITE_LOG() << "        Added catch clause deletion from "
                        << catchRange.printToString(sourceManager) << " (priority " << deletePriority << ")\n";
            }

            REWRITE_LOG() << "=== END COLLECTING TRY-CATCH REPLACEMENTS ===\n\n";
        }

        REWRITE_LOG() << "  Collected " << tryCatchBlocks.size() << " try-catch replacements\n";
    }

    void applyReplacements() {
        // Collect all insertion-style replacements into global vector
        collectCoroutineStatementReplacements();
        // Note: collectDestructorInsertions() is no longer needed since we use scopeEndReplacements
        // Note: ranged-for footer insertions will be collected in collectRangedForLoopReplacements

        // Apply range-style replacements first (declarations and references)
        collectRangeReplacements();

        // Apply ranged-for loop bulk replacements (this also collects footer insertions)
        collectRangedForLoopReplacements();

        // Apply try-catch block replacements
        collectTryCatchReplacements();

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
            const auto& [range, repl, isReplace] = declReplacement;
            int declPriority = sourceManager.getFileOffset(range.getBegin());
            globalReplacements.emplace_back(range, repl, declPriority, isReplace);
        }

        // TODO<joka921> Code duplication
        // Add reference replacements with priorities
        for (const auto &refReplacement: refReplacements) {
            // Use file offset as priority for references
            const auto& [range, repl, isReplace] = refReplacement;
            // TODO wrong names because of duplication.
            int declPriority = sourceManager.getFileOffset(range.getBegin());
            globalReplacements.emplace_back(range, repl, declPriority, isReplace);
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
                         [&](auto &a,
                             const auto &b) {
                             const auto& [rangeA, replA, priorityA, isReplaceA] = a;
                             const auto& [rangeB, replB, priorityB, isReplaceB] = b;
                             unsigned offsetA = sourceManager.getFileOffset(rangeA.getBegin());
                             unsigned offsetB = sourceManager.getFileOffset(rangeB.getBegin());

                             if (offsetA != offsetB) {
                                 return offsetA > offsetB; // Reverse order for position safety
                             }

                             // Same position - sort by priority in ascending order
                             if (priorityA != priorityB) {
                                 return priorityA < priorityB;
                             }

                             if (isReplaceA != isReplaceB) {
                                 llvm::errs() << "Trying to insert at the same position, this shouldn't happen. " <<  "\n";
                             }
                             return std::get<1>(a) < std::get<1>(b); // Sort by replacement text in ascending order
                         });
        // Deduplicate, such that the arbitrary redundant visits don't appear.
        globalReplacements.erase(std::unique(globalReplacements.begin(), globalReplacements.end()),
                                 globalReplacements.end());

        auto isEmptyRange = [](const SourceManager &SM, SourceRange Range) {
            SourceLocation Begin = SM.getSpellingLoc(Range.getBegin());
            SourceLocation End = SM.getSpellingLoc(Range.getEnd());
            return Begin == End;
        };
        auto doRewrite = [&](const auto &buf) {
            //bool isEmpty = isEmptyRange(sourceManager, std::get<0>(buf));
            bool isEmpty = !std::get<3>(buf);
            REWRITE_LOG() << " Applying at " << std::get<0>(buf).printToString(
                        sourceManager)
                    << " (priority " << std::get<2>(buf) << "), empty: " << isEmpty << ": '" << std::get<1>(buf) <<
                    "'\n";
            if (isEmpty) {
                rewriter.InsertText(std::get<0>(buf).getBegin(), std::get<1>(buf));
            } else {
                rewriter.ReplaceText(std::get<0>(buf), std::get<1>(buf));
            }
        };
        // Apply all replacements in the determined order
        std::optional<std::tuple<SourceRange, std::string, int, bool> > buffer;
        for (size_t i = 0; i < globalReplacements.size(); ++i) {
            auto &replacement = globalReplacements[i];

            if (!buffer.has_value()) {
                buffer = std::move(replacement);
                continue;
            }

            // Check if this replacement should be combined with the buffered one
            bool shouldCombine = false;
            SourceRange combinedRange = std::get<0>(*buffer);

            if (std::get<0>(*buffer) == std::get<0>(replacement)) {
                // Exact same range
                shouldCombine = true;
            } else {
                SourceLocation bufferBegin = std::get<0>(*buffer).getBegin();
                SourceLocation bufferEnd = std::get<0>(*buffer).getEnd();
                SourceLocation replBegin = std::get<0>(replacement).getBegin();
                SourceLocation replEnd = std::get<0>(replacement).getEnd();

                if (bufferBegin == replBegin) {
                    // Same start position - combine and use the wider range
                    shouldCombine = true;
                    if (sourceManager.isBeforeInTranslationUnit(bufferEnd, replEnd)) {
                        combinedRange.setEnd(replEnd);
                    }
                } else if (bufferEnd == replBegin) {
                    // Adjacent - replacement starts where buffer ends
                    shouldCombine = true;
                    combinedRange.setEnd(replEnd);
                }
            }

            if (!shouldCombine) {
                // Different source range - apply the buffered replacement
                doRewrite(*buffer);
                buffer = std::move(replacement);
                continue;
            }

            // Same or overlapping source range - combine the replacements (higher priority first)
            std::get<0>(*buffer) = combinedRange;
            std::get<1>(*buffer) += std::get<1>(replacement);
            // If any replacement is a true replacement (not insertion), mark the combined result as replacement
            if (std::get<3>(replacement)) {
                std::get<3>(*buffer) = true;
            }
            REWRITE_LOG() << "    [" << i << "] Combined with priority " << std::get<2>(replacement)
                    << ": '" << std::get<1>(replacement) << "', isReplace=" << std::get<3>(replacement)
                    << ", combinedRange=" << combinedRange.printToString(sourceManager) << "\n";
        }

        // Apply the final buffered replacement
        if (buffer.has_value()) {
            doRewrite(*buffer);
            rewriter.ReplaceText(std::get<0>(*buffer), std::get<1>(*buffer));
        }

        REWRITE_LOG() << "  Applied " << globalReplacements.size() << " replacements with priority sorting\n";
        REWRITE_LOG() << "=== END APPLYING ALL REPLACEMENTS ===\n\n";
    }

    /*
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

    */
    const std::vector<RangedForLoop> &getRangedForLoops() const {
        return rangedForLoops;
    }

    const std::vector<TryCatchBlock> &getTryCatchBlocks() const {
        return tryCatchBlocks;
    }

    const std::vector<CoroutineStatement> &getCoroutineStatements() const {
        return coroutineStatements;
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

    bool collectLocalVariables(const Stmt *body, std::set<LocalVariable> &variables) {
        LocalVariableCollector collector(variables, sourceManager, *astContext);
        collector.TraverseStmt(const_cast<Stmt *>(body));

        // Check for variable name collisions
        if (collector.hasVariableNameCollisions()) {
            DiagnosticsEngine &diags = astContext->getDiagnostics();
            collector.reportCollisions(diags);
            return false; // Collision detected, cannot rewrite
        }

        return true; // No collisions, safe to rewrite
    }

    void collectFunctionParameters(const FunctionDecl *funcDecl, std::vector<FunctionParameter> &parameters) {
        REWRITE_LOG() << "  DEBUG: Collecting function parameters\n";

        for (const auto *param: funcDecl->parameters()) {
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

    void collectMemberFunctionInfo(const FunctionDecl *funcDecl, CoroutineInfo &coro) {
        if (const auto *methodDecl = dyn_cast<CXXMethodDecl>(funcDecl)) {
            coro.isMemberFunction = true;
            coro.isConstMemberFunction = methodDecl->isConst();

            if (const auto *recordDecl = methodDecl->getParent()) {
                coro.className = recordDecl->getQualifiedNameAsString();
            }

            REWRITE_LOG() << "  DEBUG: Member function detected\n";
            REWRITE_LOG() << "    Class: " << coro.className << "\n";
            REWRITE_LOG() << "    Is const: " << (coro.isConstMemberFunction ? "yes" : "no") << "\n";
        } else {
            coro.isMemberFunction = false;
            REWRITE_LOG() << "  DEBUG: Free function (not a member function)\n";
        }
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

        // Add __self member for member functions (must be first)
        if (coro.isMemberFunction) {
            structCode += "    // Member function 'this' pointer\n";
            std::string selfType;
            if (coro.isConstMemberFunction) {
                selfType = "const " + coro.className + "*";
            } else {
                selfType = coro.className + "*";
            }
            structCode += "    " + selfType + " __self;\n\n";
            REWRITE_LOG() << "    Added __self member to struct: " << selfType << " __self\n";
        }

        // Add function parameters
        if (!coro.parameters.empty()) {
            structCode += "    // Function parameters\n";
            for (const auto &param: coro.parameters) {
                std::string paramType = "decltype(" + param.name + ")";
                structCode += "    " + paramType + " " + param.name + ";\n";
                REWRITE_LOG() << "    Added parameter to struct: " << paramType << " " << param.name << "\n";
            }
            structCode += "\n";
        }

        // Add all local variables (including ranged-for variables)
        // Use declaration location to deduplicate and handle shadowing
        if (coro.localVariables.empty()) {
            structCode += "    // No local variables found in this coroutine\n";
        } else {
            structCode += "    // Local variables (including ranged-for loop variables)\n";
            std::set<SourceLocation> addedDeclLocations; // Track which declarations we've already added

            for (const auto &var: coro.localVariables) {
                // Skip if we've already added this exact variable declaration
                if (addedDeclLocations.count(var.location) > 0) {
                    REWRITE_LOG() << "    Skipping true duplicate variable: " << var.name
                                 << " at " << var.location.printToString(sourceManager) << "\n";
                    continue;
                }
                addedDeclLocations.insert(var.location);

                // Get the member name from the pre-built mapping in CoroutineInfo
                std::string memberName = var.name; // Default to original name
                auto it = coro.declLocationToMemberName.find(var.location);
                if (it != coro.declLocationToMemberName.end()) {
                    memberName = it->second;
                }

                structCode += "    _coro_storage<" + var.referenceType + ", " + (var.isOwning
                            ? std::string{"true"}
                            : std::string{"false"}) + "> " + memberName +
                        ";\n";

                REWRITE_LOG() << "    Added variable: " << memberName << " (original name: " << var.name
                             << ", location: " << var.location.printToString(sourceManager) << ")\n";
            }
        }

        // Add storage for subexpression temporaries
        // Collect all unique temporaries from all coroutine statements
        std::set<std::string> addedTemporaries; // Track which temporaries we've already added
        bool hasTemporaries = false;
        for (const auto &coroStmt : coro.coroutineStatements) {
            if (!coroStmt.temporaries.empty()) {
                hasTemporaries = true;
                break;
            }
        }

        if (hasTemporaries) {
            structCode += "\n    // Subexpression temporaries\n";
            for (const auto &coroStmt : coro.coroutineStatements) {
                for (const auto &temp : coroStmt.temporaries) {
                    // Only add each temporary once
                    if (addedTemporaries.find(temp.tempVarName) == addedTemporaries.end()) {
                        addedTemporaries.insert(temp.tempVarName);

                        // Get the canonical type string for the temporary
                        std::string typeStr = typeAsString(temp.type, *astContext);

                        // All subexpression temporaries are owning (they store the actual object)
                        // and we access them by reference
                        structCode += "    _coro_storage<" + typeStr + "&, true> " + temp.tempVarName + ";\n";

                        REWRITE_LOG() << "    Added temporary to struct: _coro_storage<" << typeStr << "&, true> " << temp.tempVarName << "\n";
                    }
                }
            }
        }

        // Add yield buffer for temporaries if needed
        if (!coro.yieldedOrAwaitedTemporaries.empty()) {
            structCode += "\n    // Buffer for yielded/awaited temporaries\n";
            
            // Calculate max size and alignment
            size_t maxSize = 0;
            size_t maxAlignment = 1;
            
            REWRITE_LOG() << "  DEBUG: Calculating buffer size for " << coro.yieldedOrAwaitedTemporaries.size() << " temporary types:\n";
            
            for (const auto &tempType : coro.yieldedOrAwaitedTemporaries) {
                // Get type string for logging
                std::string typeStr = typeAsString(tempType, *astContext);
                REWRITE_LOG() << "    Temporary type: " << typeStr << "\n";
                
                // Calculate size and alignment (we'll use sizeof and alignof expressions)
                // Note: We can't calculate actual sizes at compile time of the rewriter,
                // so we'll generate code that calculates it at compile time of the target
            }
            
            // Generate buffer with max size/alignment using template metaprogramming
            structCode += "    alignas(std::ranges::max(std::array{std::size_t{1}, ";
            bool first = true;
            for (const auto &tempType : coro.yieldedOrAwaitedTemporaries) {
                if (!first) structCode += ", ";
                first = false;
                std::string typeStr = typeAsString(tempType, *astContext);
                structCode += "alignof(" + typeStr + ")";
            }
            structCode += "})) ";

            structCode += "char yieldBuffer[std::ranges::max(std::array{std::size_t{1}, ";
            first = true;
            for (const auto &tempType : coro.yieldedOrAwaitedTemporaries) {
                if (!first) structCode += ", ";
                first = false;
                std::string typeStr = typeAsString(tempType, *astContext);
                structCode += "sizeof(" + typeStr + ")";
            }
            structCode += "})];\n";
            
            REWRITE_LOG() << "    Added yieldBuffer to struct with max size/alignment of " << coro.yieldedOrAwaitedTemporaries.size() << " types\n";
        }

        // Add exception handling infrastructure
        if (!coro.tryCatchBlocks.empty()) {
            structCode += "\n    // Exception handling infrastructure\n";
            structCode += "    std::vector<size_t> activeTryBlocks;\n";

            // Add handleException function
            structCode += "\n    void handleException(std::exception_ptr eptr, size_t& nextState, std::function<void()> resume) {\n";
            structCode += "      nextState = dispatchExceptionHandling(std::move(eptr));\n";
            structCode += "      resume();\n";
            structCode += "    }\n";

            // Add dispatchExceptionHandling function
            structCode += "\n    size_t dispatchExceptionHandling(std::exception_ptr eptr) {\n";
            structCode += "      switch (activeTryBlocks.back()) {\n";
            for (const auto &tryCatch : coro.tryCatchBlocks) {
                structCode += "        case " + std::to_string(tryCatch.index) + ": return catchClauseImpl_" + std::to_string(tryCatch.index) + "(std::move(eptr));\n";
            }
            structCode += "        default: std::terminate();\n";
            structCode += "      }\n";
            structCode += "    }\n";

            // Add catch clause implementation member functions
            structCode += "\n    // Exception handler member functions\n";
            for (const auto &tryCatch : coro.tryCatchBlocks) {
                structCode += "    size_t catchClauseImpl_" + std::to_string(tryCatch.index) + "(std::exception_ptr eptr) {\n";
                structCode += "      auto nextState = activeTryBlocks.back();\n";
                structCode += "      activeTryBlocks.pop_back();\n";
                structCode += "      auto lambda = [&]() {\n";
                structCode += "        try {\n";
                structCode += "          std::rethrow_exception(eptr);\n";
                structCode += "        } ";

                // Add all catch clauses
                for (const auto &catchClause : tryCatch.catchClauses) {
                    structCode += catchClause + " ";
                }

                structCode += "\n        return nextState;\n";
                structCode += "      };\n";
                structCode += "      if (activeTryBlocks.empty()) {\n";
                structCode += "        return lambda();\n";
                structCode += "      } else {\n";
                structCode += "        try {\n";
                structCode += "          return lambda();\n";
                structCode += "        } catch (...) {\n";
                structCode += "          return dispatchExceptionHandling(std::current_exception());\n";
                structCode += "        }\n";
                structCode += "      }\n";
                structCode += "    }\n";

                REWRITE_LOG() << "    Added catchClauseImpl_" << tryCatch.index << " to struct\n";
            }

            // Add destroyBecauseOfException function
            structCode += "\n    // Destroy variables in case of exception in try block\n";
            structCode += "    void destroyBecauseOfException(size_t tryCatchBlockIndex) {\n";
            structCode += "      switch (tryCatchBlockIndex) {\n";
            for (const auto &tryCatch : coro.tryCatchBlocks) {
                structCode += "        case " + std::to_string(tryCatch.index) + ":\n";
                // Destroy all variables in this try block in reverse order
                for (const auto &varName : tryCatch.variablesInTryBlock) {
                    structCode += "          if (state." + varName + ".constructed) state." + varName + ".destroy();\n";
                }
                structCode += "          break;\n";
            }
            structCode += "        default: break;\n";
            structCode += "      }\n";
            structCode += "    }\n";
            REWRITE_LOG() << "    Added destroyBecauseOfException function\n";
        }

        // Add destroySuspendedCoro function (always needed if there are any coroutine statements)
        if (!coro.coroutineStatements.empty()) {
            structCode += "\n    // Destroy variables when coroutine is suspended at a specific state\n";
            structCode += "    void destroySuspendedCoro(size_t curState) {\n";
            structCode += "      switch (curState) {\n";

            // Generate cases in descending order (from highest index to lowest)
            // This allows us to use fallthrough and goto for proper destruction order
            std::vector<const CoroutineStatement*> sortedStmts;
            for (const auto &stmt : coro.coroutineStatements) {
                sortedStmts.push_back(&stmt);
            }
            std::sort(sortedStmts.begin(), sortedStmts.end(),
                     [](const CoroutineStatement* a, const CoroutineStatement* b) {
                         return a->index > b->index;  // Descending order
                     });

            for (size_t i = 0; i < sortedStmts.size(); ++i) {
                const CoroutineStatement &currentStmt = *sortedStmts[i];
                structCode += "        case " + std::to_string(currentStmt.index) + ":\n";

                // Find the next state we'll process (next in sorted list = next lower index)
                const CoroutineStatement *nextStmt = (i + 1 < sortedStmts.size()) ? sortedStmts[i + 1] : nullptr;

                // Determine which variables to destroy: those in current but NOT in next
                // These are the variables that were added since the previous state
                std::vector<std::string> varsToDestroy;
                for (const auto &varName : currentStmt.aliveVariables) {
                    bool inNext = false;
                    if (nextStmt) {
                        for (const auto &nextVar : nextStmt->aliveVariables) {
                            if (varName == nextVar) {
                                inNext = true;
                                break;
                            }
                        }
                    }
                    if (!inNext) {
                        varsToDestroy.push_back(varName);
                    }
                }

                // Destroy only the new variables
                for (const auto &varName : varsToDestroy) {
                    structCode += "          " + varName + ".destroy();\n";
                }

                // Find the target state to continue with:
                // The first earlier state whose alive variables are ALL contained in current state
                const CoroutineStatement *targetStmt = nullptr;
                for (size_t j = i + 1; j < sortedStmts.size(); ++j) {
                    const CoroutineStatement *candidateStmt = sortedStmts[j];

                    // Check if ALL variables in candidate are also in current
                    bool allContained = true;
                    for (const auto &candidateVar : candidateStmt->aliveVariables) {
                        bool found = false;
                        for (const auto &currentVar : currentStmt.aliveVariables) {
                            if (candidateVar == currentVar) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            allContained = false;
                            break;
                        }
                    }

                    if (allContained) {
                        targetStmt = candidateStmt;
                        break;  // Found the first matching state
                    }
                }

                // Determine if we need goto or fallthrough
                if (targetStmt) {
                    // Check if target is the immediately next state
                    if (nextStmt && targetStmt->index == nextStmt->index) {
                        // Fall through naturally to the next state
                    } else {
                        // Jump to the target state's cleanup label
                        structCode += "          goto cleanup_" + std::to_string(targetStmt->index) + ";\n";
                    }
                } else {
                    // No more states to process
                    structCode += "          break;\n";
                }

                // Add cleanup label if this is a potential goto target
                if (i + 1 < sortedStmts.size()) {
                    const CoroutineStatement &nextStmt = *sortedStmts[i + 1];
                    structCode += "        cleanup_" + std::to_string(nextStmt.index) + ":\n";
                }
            }

            structCode += "        case 0:  // initial state\n";
            structCode += "          break;\n";
            structCode += "      }\n";
            structCode += "    }\n";
            REWRITE_LOG() << "    Added destroySuspendedCoro function\n";
        }

        structCode += "  };\n\n";

        // Add typedef to avoid comma issues in macro call
        structCode += "  using _ActualCoroType = " + returnType + ";\n";

        // Generate the COROUTINE_HEADER macro call
        // The coroutine body will follow immediately after this
        if (!coro.tryCatchBlocks.empty()) {
            structCode += "  COROUTINE_HEADER_WITH_TRY(_ActualCoroType, _detail_coro_impl) ";
        } else {
            structCode += "  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl) ";
        }

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
            CoroutineInfo coro;
            coro.function = funcDecl;

            REWRITE_LOG() << "Processing coroutine: " << funcDecl->getQualifiedNameAsString() << "\n";

            // Check for variable name collisions - skip rewriting if found
            if (!collectLocalVariables(funcDecl->getBody(), coro.localVariables)) {
                REWRITE_LOG() << "Skipping coroutine " << funcDecl->getQualifiedNameAsString()
                        << " due to variable name collisions\n";
                return true; // Skip this coroutine
            }

            collectFunctionParameters(funcDecl, coro.parameters);
            collectMemberFunctionInfo(funcDecl, coro);

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

    bool VisitLambdaExpr(LambdaExpr *lambdaExpr) {
        // Get the call operator (the lambda body function)
        const CXXMethodDecl *callOperator = lambdaExpr->getCallOperator();
        if (!callOperator->hasBody()) {
            return true;
        }

        auto fileId = sourceManager.getFileID(lambdaExpr->getBeginLoc());
        if (fileId != sourceManager.getMainFileID()) {
            return true;
        }

        if (containsCoroutineKeywords(callOperator->getBody())) {
            // Check for captures - we don't support lambda coroutines with captures yet
            unsigned numCaptures = lambdaExpr->capture_size();

            if (numCaptures > 0) {
                // Report error for lambda coroutines with captures
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Error,
                    "coroutine lambdas with captures are not supported");
                diagnosticsEngine.Report(lambdaExpr->getBeginLoc(), diagID);

                // Also report each capture location for clarity
                unsigned noteID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Note,
                    "capture found here");
                for (const auto& capture : lambdaExpr->captures()) {
                    diagnosticsEngine.Report(capture.getLocation(), noteID);
                }

                REWRITE_LOG() << "Skipping lambda coroutine due to captures\n";
                return true; // Skip this lambda coroutine
            }

            CoroutineInfo coro;
            coro.function = callOperator;
            coro.isLambda = true;
            coro.lambdaExpr = lambdaExpr;

            REWRITE_LOG() << "Processing lambda coroutine\n";

            // Check for variable name collisions - skip rewriting if found
            if (!collectLocalVariables(callOperator->getBody(), coro.localVariables)) {
                REWRITE_LOG() << "Skipping lambda coroutine due to variable name collisions\n";
                return true; // Skip this lambda coroutine
            }

            // Collect lambda parameters - they need to be added to the impl struct
            collectFunctionParameters(callOperator, coro.parameters);

            // Lambda coroutines are not member functions of classes in the traditional sense
            // collectMemberFunctionInfo is not needed for lambdas

            coro.insertionPoint = findStructInsertionPoint(callOperator);
            if (coro.insertionPoint.isInvalid()) {
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Warning,
                    "Could not find insertion point for lambda _detail_coro_impl struct");
                diagnosticsEngine.Report(lambdaExpr->getBeginLoc(), diagID);
                coro.hasError = true;

                // Output the struct code to console as fallback
                std::string structCode = generateCoroImplStruct(coro);
                REWRITE_LOG() << "WARNING: Could not insert lambda struct, here's what would have been inserted:\n";
                REWRITE_LOG() << "========== STRUCT CODE ==========\n";
                REWRITE_LOG() << structCode;
                REWRITE_LOG() << "==================================\n";
            }

            coroutines.push_back(coro);

            REWRITE_LOG() << "  Found " << coro.localVariables.size() << " local variables in lambda\n";
            REWRITE_LOG() << "  Type: Lambda coroutine\n";
        }

        return true;
    }

    void updateFunctionReturnType(const CoroutineInfo &coro) {
        if (coro.isLambda) {
            REWRITE_LOG() << "Updating lambda return type\n";
            updateLambdaReturnType(coro);
            return;
        }

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

    void updateLambdaReturnType(const CoroutineInfo &coro) {
        if (!coro.lambdaExpr) {
            REWRITE_LOG() << "  ERROR: No lambda expression to update\n";
            return;
        }

        // Check if lambda has an explicit trailing return type
        if (!coro.lambdaExpr->hasExplicitResultType()) {
            unsigned diagID = diagnosticsEngine.getCustomDiagID(
                clang::DiagnosticsEngine::Error,
                "coroutine lambda must have an explicit trailing return type");
            diagnosticsEngine.Report(coro.lambdaExpr->getBeginLoc(), diagID);
            return;
        }

        // Find and delete the trailing return type by searching for "-> ReturnType"
        SourceRange lambdaRange = coro.lambdaExpr->getSourceRange();
        SourceLocation lambdaStart = lambdaRange.getBegin();
        SourceLocation lambdaEnd = lambdaRange.getEnd();
        
        // Get the source text of the entire lambda
        auto txt = rewriter.getRewrittenText(lambdaRange);
        StringRef lambdaText = txt;
        if (lambdaText.empty()) {
            // Fallback: get original source text
            bool invalid = false;
            lambdaText = sourceManager.getCharacterData(lambdaStart, &invalid);
            if (invalid) {
                REWRITE_LOG() << "  ERROR: Cannot get lambda source text\n";
                return;
            }
            
            // Calculate length manually
            unsigned startOffset = sourceManager.getFileOffset(lambdaStart);
            unsigned endOffset = sourceManager.getFileOffset(lambdaEnd);
            if (endOffset <= startOffset) {
                REWRITE_LOG() << "  ERROR: Invalid lambda range\n";
                return;
            }
            lambdaText = lambdaText.substr(0, endOffset - startOffset + 1);
        }
        
        // Look for "-> " pattern in the lambda text
        size_t arrowPos = lambdaText.find("-> ");
        if (arrowPos == StringRef::npos) {
            REWRITE_LOG() << "  ERROR: Cannot find trailing return type arrow\n";
            return;
        }
        
        // Find the opening brace after the return type
        size_t bracePos = lambdaText.find('{', arrowPos);
        if (bracePos == StringRef::npos) {
            REWRITE_LOG() << "  ERROR: Cannot find lambda body opening brace\n";
            return;
        }
        
        // Calculate the range to delete: from "-> " to just before the '{'
        SourceLocation deleteStart = lambdaStart.getLocWithOffset(arrowPos);
        SourceLocation deleteEnd = lambdaStart.getLocWithOffset(bracePos - 1);
        
        // Delete the trailing return type (including the "-> " part)
        SourceRange deleteRange(deleteStart, deleteEnd);
        rewriter.RemoveText(deleteRange);
        
        REWRITE_LOG() << "  Deleted lambda trailing return type from position " << arrowPos 
                      << " to " << (bracePos - 1) << "\n";
    }

    void performRewrites() {
        for (auto &coro: coroutines) {
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

    void rewriteCoroutineBody(CoroutineInfo &coro) {
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
            const CXXRecordDecl *classRecord = nullptr;
            if (coro.isMemberFunction) {
                if (const auto *methodDecl = dyn_cast<CXXMethodDecl>(coro.function)) {
                    classRecord = methodDecl->getParent();
                }
            }
            CoroutineBodyRewriter initialRewriter(coro.localVariables, rewriter, sourceManager,
                                                  coro, *astContext, coro.isMemberFunction, classRecord);
            initialRewriter.TraverseStmt(const_cast<Stmt *>(bodyStmt));

            // Get the ranged-for loops and add them to the coroutine info for struct generation
            const auto &rangedForLoops = initialRewriter.getRangedForLoops();
            const_cast<CoroutineInfo &>(coro).rangedForLoops = rangedForLoops;
            REWRITE_LOG() << "  DEBUG: Found " << rangedForLoops.size() << " ranged-for loops\n";

            // Get the try-catch blocks and add them to the coroutine info for struct generation
            const auto &tryCatchBlocks = initialRewriter.getTryCatchBlocks();
            const_cast<CoroutineInfo &>(coro).tryCatchBlocks = tryCatchBlocks;
            REWRITE_LOG() << "  DEBUG: Found " << tryCatchBlocks.size() << " try-catch blocks\n";

            // Get the coroutine statements (suspension points) and add them to the coroutine info
            const auto &coroutineStatements = initialRewriter.getCoroutineStatements();
            const_cast<CoroutineInfo &>(coro).coroutineStatements = coroutineStatements;
            REWRITE_LOG() << "  DEBUG: Found " << coroutineStatements.size() << " suspension points\n";

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

                auto addBeginAndEndVar = [&](const std::string &beginOrEnd, const std::string &name, int priority) {
                    LocalVariable beginVar;
                    beginVar.name = name;
                    beginVar.type = "std::decay_t<decltype(std::declval<" + rangeVar.referenceType + ">()." + beginOrEnd
                                    + "())>";
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

            CoroutineBodyRewriter finalRewriter(coro.localVariables, rewriter, sourceManager,
                                                coro, *astContext, coro.isMemberFunction, classRecord);
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

    /*
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
    */

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

                // For member functions, 'this' must be the first argument
                if (coro.isMemberFunction) {
                    paramList = "this";
                }

                // Add function parameters
                if (!coro.parameters.empty()) {
                    for (size_t i = 0; i < coro.parameters.size(); ++i) {
                        if (!paramList.empty()) paramList += ", ";
                        paramList += coro.parameters[i].name;
                    }
                }

                // Add CO_RETURN_FALLOFF before COROUTINE_FOOTER
                // Calculate the falloff index - it's the next available coroutine statement index
                unsigned falloffIndex = coro.coroutineStatements.empty() ? 1 :
                    std::max_element(coro.coroutineStatements.begin(), coro.coroutineStatements.end(),
                        [](const CoroutineStatement& a, const CoroutineStatement& b) {
                            return a.index < b.index;
                        })->index + 1;

                ScopeEndReplacement falloffReplacement;
                falloffReplacement.replacement = "CO_RETURN_FALLOFF(" + std::to_string(falloffIndex) + ");\n";
                falloffReplacement.priority = std::numeric_limits<int>::max() - 1; // Just before COROUTINE_FOOTER
                body_rewriter.scopeEndReplacements[fileOffset].push_back(falloffReplacement);

                REWRITE_LOG() << "  DEBUG: Added CO_RETURN_FALLOFF(" << falloffIndex << ") with priority "
                        << falloffReplacement.priority << "\n";

                // Choose the appropriate footer macro based on whether we have try-catch blocks
                std::string footerMacro = coro.tryCatchBlocks.empty() ? "COROUTINE_FOOTER" : "COROUTINE_FOOTER_WITH_TRY";

                if (!paramList.empty()) {
                    replacement.replacement = footerMacro + "(" + paramList + ")\n";
                } else {
                    replacement.replacement = footerMacro + "()\n";
                }

                // Use maximum priority to ensure COROUTINE_FOOTER comes after all destructors and CO_RETURN_FALLOFF
                replacement.priority = std::numeric_limits<int>::max();

                body_rewriter.scopeEndReplacements[fileOffset].push_back(replacement);

                REWRITE_LOG() << "  DEBUG: Added " << footerMacro << " to scope end replacements with maximum priority ("
                        << replacement.priority << ") at file offset " << fileOffset << "\n";
                REWRITE_LOG() << "  DEBUG: " << footerMacro << " replacement text: '" << replacement.replacement << "'\n";

                REWRITE_LOG() << "  DEBUG: Successfully added " << footerMacro << " to scope end system\n";
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

