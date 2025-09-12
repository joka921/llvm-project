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

using namespace clang::tooling;
using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;

static llvm::cl::OptionCategory MyToolCategory("coroutine-rewriter");
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::extrahelp MoreHelp("\nRewrites C++20 coroutines to C++17 compatible state machines.\n");

struct LocalVariable {
    std::string name;
    std::string type;
    SourceLocation location;
    
    bool operator<(const LocalVariable& other) const {
        return name < other.name;
    }
};

struct RangedForLoop {
    const CXXForRangeStmt* stmt;
    std::string loopVarName;
    std::string loopVarType;
    std::string rangeExpr;
    std::string rangeVarName;   // e.g., "__range_0"
    std::string beginVarName;   // e.g., "__begin_0"
    std::string endVarName;     // e.g., "__end_0"
    unsigned index;
    SourceRange fullRange;
};

struct CoroutineInfo {
    const FunctionDecl* function;
    std::set<LocalVariable> localVariables;
    std::vector<RangedForLoop> rangedForLoops; // Add ranged-for loop info
    SourceLocation insertionPoint;
    bool hasError = false;
};

class LocalVariableCollector : public RecursiveASTVisitor<LocalVariableCollector> {
private:
    std::set<LocalVariable>& variables;
    const SourceManager& sourceManager;
    ASTContext& astContext;

public:
    LocalVariableCollector(std::set<LocalVariable>& vars, const SourceManager& SM, ASTContext& ctx) 
        : variables(vars), sourceManager(SM), astContext(ctx) {}

    std::string getFullyQualifiedTypeName(QualType type) {
        // Get the canonical type (resolves typedefs, auto, etc.)
        QualType canonicalType = type.getCanonicalType();
        
        // Use a printing policy that produces fully qualified names
        PrintingPolicy policy(astContext.getLangOpts());
        policy.SuppressScope = false;  // Include scope information
        policy.SuppressTagKeyword = false;  // Keep 'struct', 'class', etc.
        policy.SuppressUnwrittenScope = false;  // Include all scopes
        policy.FullyQualifiedName = true;  // Force fully qualified names
        policy.PrintCanonicalTypes = true;  // Print canonical types
        
        return canonicalType.getAsString(policy);
    }

    bool VisitDeclStmt(DeclStmt* declStmt) {
        for (auto* decl : declStmt->decls()) {
            if (auto* varDecl = dyn_cast<VarDecl>(decl)) {
                if (!varDecl->getType()->isFunctionType() && 
                    !varDecl->getType()->isReferenceType()) {
                    LocalVariable var;
                    var.name = varDecl->getNameAsString();
                    var.type = getFullyQualifiedTypeName(varDecl->getType());
                    var.location = varDecl->getLocation();
                    
                    if (!var.name.empty()) {
                        variables.insert(var);
                        std::cout << "  Found local variable: " << var.type << " " << var.name << "\n";
                    }
                }
            }
        }
        return true;
    }
};

struct ScopeInfo {
    const CompoundStmt* compoundStmt;
    std::vector<std::string> variablesInScope;
    SourceLocation scopeEnd;
};

struct CoroutineStatement {
    enum Type { YIELD, AWAIT };
    Type type;
    const Stmt* stmt;  // CoawaitExpr* or CoyieldExpr*
    const Expr* operand; // The expression being yielded/awaited
    unsigned index;
    SourceLocation keywordLoc;
    SourceLocation operandStart;
    SourceLocation operandEnd;
};


class CoroutineBodyRewriter : public RecursiveASTVisitor<CoroutineBodyRewriter> {
private:
    const std::set<LocalVariable>& localVariables;
    Rewriter& rewriter;
    const SourceManager& sourceManager;
    std::set<std::string> variableNames;
    std::vector<std::pair<SourceRange, std::string>> declReplacements;
    std::vector<std::pair<SourceRange, std::string>> refReplacements;
    std::set<SourceLocation> processedDeclarations;
    std::vector<ScopeInfo> scopeStack;
    std::vector<std::pair<SourceLocation, std::string>> destructorInsertions;
    std::vector<CoroutineStatement> coroutineStatements;
    unsigned nextCoroStatementIndex;
    std::vector<RangedForLoop> rangedForLoops;
    unsigned nextRangedForIndex;
    
    // Global replacement vector for ALL types of replacements
public:
    std::vector<std::pair<SourceRange, std::string>> globalReplacements;

public:
    CoroutineBodyRewriter(const std::set<LocalVariable>& vars, Rewriter& rewr, const SourceManager& SM)
        : localVariables(vars), rewriter(rewr), sourceManager(SM), nextCoroStatementIndex(1), nextRangedForIndex(0) {
        // Create a set of variable names for quick lookup
        for (const auto& var : localVariables) {
            variableNames.insert(var.name);
        }
    }

    // Track compound statements (scopes)
    bool VisitCompoundStmt(CompoundStmt* compoundStmt) {
        std::cout << "  DEBUG: Entering scope (CompoundStmt)\n";
        
        // Create scope info
        ScopeInfo scope;
        scope.compoundStmt = compoundStmt;
        scope.scopeEnd = compoundStmt->getRBracLoc();
        
        // Push scope onto stack
        scopeStack.push_back(scope);
        
        // Traverse children manually to have control over when we pop the scope
        for (auto* child : compoundStmt->children()) {
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
        
        std::cout << "  DEBUG: Exiting scope (CompoundStmt)\n";
        
        return false; // We handled traversal manually
    }

    // Handle variable declarations - collect for later processing
    bool VisitDeclStmt(DeclStmt* declStmt) {
        for (auto* decl : declStmt->decls()) {
            if (auto* varDecl = dyn_cast<VarDecl>(decl)) {
                std::string varName = varDecl->getNameAsString();
                
                if (variableNames.count(varName)) {
                    std::cout << "  Found variable declaration: " << varName << "\n";
                    
                    SourceLocation declLoc = varDecl->getLocation();
                    if (processedDeclarations.count(declLoc)) {
                        continue; // Already processed this declaration
                    }
                    processedDeclarations.insert(declLoc);
                    
                    // Add variable to current scope
                    if (!scopeStack.empty()) {
                        scopeStack.back().variablesInScope.push_back(varName);
                        std::cout << "    DEBUG: Added variable '" << varName << "' to current scope\n";
                    }
                    
                    // Build construct call  
                    std::string constructCall = "this->state." + varName + ".construct(";
                    
                    if (varDecl->hasInit()) {
                        std::string initCode = getInitializationArguments(varDecl);
                        constructCall += initCode;
                    }
                    
                    constructCall += ");";
                    
                    // Replace the entire declaration
                    SourceRange declRange = varDecl->getSourceRange();
                    declReplacements.emplace_back(declRange, constructCall);
                }
            }
        }
        return true; // Continue traversing
    }

    // Handle co_yield expressions
    bool VisitCoyieldExpr(CoyieldExpr* coyield) {
        std::cout << "  Found co_yield expression\n";
        
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
        
        std::cout << "    DEBUG: Added co_yield with index " << coroStmt.index << "\n";
        
        return true;
    }
    
    // Handle co_await expressions  
    bool VisitCoawaitExpr(CoawaitExpr* coawait) {
        std::cout << "  Found co_await expression\n";
        
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
        
        std::cout << "    DEBUG: Added co_await with index " << coroStmt.index << "\n";
        
        return true;
    }

    // Handle ranged-for loops
    bool VisitCXXForRangeStmt(CXXForRangeStmt* forRange) {
        std::cout << "\n=== RANGED-FOR LOOP DETECTION ===\n";
        std::cout << "  Found ranged-for loop at: " << forRange->getSourceRange().getBegin().printToString(sourceManager) 
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
        const VarDecl* loopVar = forRange->getLoopVariable();
        if (loopVar) {
            rangedFor.loopVarName = loopVar->getNameAsString();
            QualType loopVarType = loopVar->getType();
            rangedFor.loopVarType = loopVarType.getAsString();
            std::cout << "    Loop variable: " << rangedFor.loopVarType << " " << rangedFor.loopVarName 
                      << " at " << loopVar->getLocation().printToString(sourceManager) << "\n";
        }
        
        // Extract range expression
        const Expr* rangeExpr = forRange->getRangeInit();
        if (rangeExpr) {
            SourceRange rangeRange = rangeExpr->getSourceRange();
            CharSourceRange charRange = CharSourceRange::getTokenRange(rangeRange);
            rangedFor.rangeExpr = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
            std::cout << "    Range expression: '" << rangedFor.rangeExpr << "' at " 
                      << rangeRange.getBegin().printToString(sourceManager) << "\n";
        }
        
        // Extract detailed position information
        std::cout << "  DETAILED POSITIONS:\n";
        std::cout << "    For keyword: " << forRange->getForLoc().printToString(sourceManager) << "\n";
        std::cout << "    Colon location: " << forRange->getColonLoc().printToString(sourceManager) << "\n";
        std::cout << "    RParenLoc: " << forRange->getRParenLoc().printToString(sourceManager) << "\n";
        
        const Stmt* body = forRange->getBody();
        if (body) {
            std::cout << "    Body type: " << body->getStmtClassName() << "\n";
            std::cout << "    Body range: " << body->getSourceRange().getBegin().printToString(sourceManager) 
                      << " to " << body->getSourceRange().getEnd().printToString(sourceManager) << "\n";
            
            if (auto* compoundBody = dyn_cast<CompoundStmt>(body)) {
                std::cout << "    Body LBraceLoc: " << compoundBody->getLBracLoc().printToString(sourceManager) << "\n";
                std::cout << "    Body RBraceLoc: " << compoundBody->getRBracLoc().printToString(sourceManager) << "\n";
                std::cout << "    Body has " << compoundBody->size() << " child statements\n";
                
                int childIndex = 0;
                for (const auto* child : compoundBody->children()) {
                    if (child) {
                        std::cout << "      Child[" << childIndex << "]: " << child->getStmtClassName() 
                                  << " at " << child->getSourceRange().getBegin().printToString(sourceManager) 
                                  << " to " << child->getSourceRange().getEnd().printToString(sourceManager) << "\n";
                    }
                    childIndex++;
                }
            }
        }
        
        std::cout << "    Generated variable names: " << rangedFor.rangeVarName 
                  << ", " << rangedFor.beginVarName << ", " << rangedFor.endVarName << "\n";
        
        rangedForLoops.push_back(rangedFor);
        
        // CRITICAL: Must manually traverse the loop body since we're overriding VisitCXXForRangeStmt
        std::cout << "    MANUALLY traversing loop body for normal coroutine rewriting...\n";
        if (body) {
            std::cout << "      Starting manual traversal of body: " << body->getStmtClassName() << "\n";
            TraverseStmt(const_cast<Stmt*>(body));
            std::cout << "      Completed manual traversal of body\n";
        } else {
            std::cout << "      WARNING: No body to traverse\n";
        }
        
        // IMPORTANT: Return false to prevent automatic traversal (we did it manually)
        std::cout << "    Returning false to prevent double traversal\n";
        std::cout << "=== END RANGED-FOR DETECTION ===\n\n";
        
        return false;
    }

    // Handle variable references - but not in declaration contexts
    bool VisitDeclRefExpr(DeclRefExpr* declRef) {
        if (auto* varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
            std::string varName = varDecl->getNameAsString();
            
            if (variableNames.count(varName)) {
                // Check if this reference is part of a declaration we're already handling
                if (!isPartOfDeclaration(declRef)) {
                    std::cout << "  Found variable reference: " << varName << "\n";
                    
                    std::string getCall = "this->state." + varName + ".get()";
                    SourceRange refRange = declRef->getSourceRange();
                    refReplacements.emplace_back(refRange, getCall);
                }
            }
        }
        return true;
    }

private:
    std::string getInitializationArguments(const VarDecl* varDecl) {
        std::string varName = varDecl->getNameAsString();
        std::cout << "    DEBUG: Processing initialization for variable: " << varName << "\n";
        
        if (!varDecl->hasInit()) {
            std::cout << "    DEBUG: No initialization for " << varName << "\n";
            return "";
        }
        
        const Expr* init = varDecl->getInit();
        std::cout << "    DEBUG: Initialization expression type: " << init->getStmtClassName() << "\n";
        
        // Get raw text for debugging
        SourceRange range = init->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(range);
        std::string rawText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
        std::cout << "    DEBUG: Raw initialization text: '" << rawText << "'\n";
        
        // Check if this is direct list initialization (T var{args}) vs copy initialization (T var = {args})
        bool isDirectListInit = rawText.find(varName + "{") == 0;
        std::cout << "    DEBUG: Is direct list initialization (T var{...}): " << (isDirectListInit ? "true" : "false") << "\n";
        
        std::string result;
        
        if (isDirectListInit) {
            // For T var{args}, extract just the {args} part
            std::cout << "    DEBUG: Handling direct list initialization\n";
            size_t bracePos = rawText.find('{');
            if (bracePos != std::string::npos) {
                std::string listPart = rawText.substr(bracePos);
                std::cout << "    DEBUG: Extracted list part: '" << listPart << "'\n";
                
                // Still need to rewrite any variable references within the list
                result = rewriteVariableReferencesInText(listPart, varName);
            } else {
                std::cout << "    DEBUG: No brace found in direct list init, fallback to normal processing\n";
                result = processInitExpression(init, varName);
            }
        } else {
            // Handle other initialization styles normally
            result = processInitExpression(init, varName);
        }
        
        std::cout << "    DEBUG: Final initialization arguments: '" << result << "'\n";
        return result;
    }
    
    std::string processInitExpression(const Expr* init, const std::string& currentVarName) {
        // Handle different initialization styles
        if (auto* constructExpr = dyn_cast<CXXConstructExpr>(init)) {
            std::cout << "    DEBUG: Found CXXConstructExpr\n";
            return handleConstructorArgs(constructExpr);
        } else if (auto* initListExpr = dyn_cast<InitListExpr>(init)) {
            std::cout << "    DEBUG: Found InitListExpr\n";
            return handleInitListArgs(initListExpr);
        } else {
            std::cout << "    DEBUG: Using rewriteExpression for other type\n";
            return rewriteExpressionExceptVar(init, currentVarName);
        }
    }
    
    std::string rewriteVariableReferencesInText(const std::string& text, const std::string& excludeVar) {
        std::cout << "        DEBUG: rewriteVariableReferencesInText input: '" << text << "'\n";
        std::cout << "        DEBUG: Excluding variable: '" << excludeVar << "'\n";
        
        std::string result = text;
        
        // Replace variable references in the text, but exclude the current variable being declared
        for (const auto& varName : variableNames) {
            if (varName == excludeVar) {
                std::cout << "        DEBUG: Skipping replacement of '" << varName << "' (current variable)\n";
                continue; // Don't replace the variable we're currently declaring
            }
            
            std::string replacement = "this->state." + varName + ".get()";
            
            size_t pos = 0;
            while ((pos = result.find(varName, pos)) != std::string::npos) {
                // Check if it's a word boundary
                bool isWordStart = (pos == 0) || !std::isalnum(result[pos-1]);
                bool isWordEnd = (pos + varName.length() >= result.length()) || 
                                !std::isalnum(result[pos + varName.length()]);
                
                if (isWordStart && isWordEnd) {
                    std::cout << "        DEBUG: Replacing '" << varName << "' with '" << replacement << "' at position " << pos << "\n";
                    result.replace(pos, varName.length(), replacement);
                    pos += replacement.length();
                } else {
                    pos += varName.length();
                }
            }
        }
        
        std::cout << "        DEBUG: rewriteVariableReferencesInText output: '" << result << "'\n";
        return result;
    }
    
    std::string handleConstructorArgs(const CXXConstructExpr* constructExpr) {
        std::cout << "      DEBUG: handleConstructorArgs - found " << constructExpr->getNumArgs() << " arguments\n";
        std::string args;
        
        for (unsigned i = 0; i < constructExpr->getNumArgs(); ++i) {
            if (i > 0) args += ", ";
            const Expr* arg = constructExpr->getArg(i);
            std::string argText = rewriteExpression(arg);
            std::cout << "      DEBUG: Constructor arg " << i << ": '" << argText << "'\n";
            args += argText;
        }
        
        std::cout << "      DEBUG: handleConstructorArgs result: '" << args << "'\n";
        return args;
    }
    
    std::string handleInitListArgs(const InitListExpr* initList) {
        std::cout << "      DEBUG: handleInitListArgs - found " << initList->getNumInits() << " elements\n";
        std::string args = "{";
        
        for (unsigned i = 0; i < initList->getNumInits(); ++i) {
            if (i > 0) args += ", ";
            const Expr* init = initList->getInit(i);
            std::string initText = rewriteExpression(init);
            std::cout << "      DEBUG: InitList element " << i << ": '" << initText << "'\n";
            args += initText;
        }
        
        args += "}";
        std::cout << "      DEBUG: handleInitListArgs result: '" << args << "'\n";
        return args;
    }
    
    std::string rewriteExpression(const Expr* expr) {
        if (!expr) return "";
        
        SourceRange range = expr->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(range);
        std::string exprText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
        
        std::cout << "        DEBUG: rewriteExpression input: '" << exprText << "'\n";
        std::cout << "        DEBUG: Expression type: " << expr->getStmtClassName() << "\n";
        
        // Replace variable references in the expression
        std::string originalText = exprText;
        for (const auto& varName : variableNames) {
            std::string replacement = "this->state." + varName + ".get()";
            
            size_t pos = 0;
            while ((pos = exprText.find(varName, pos)) != std::string::npos) {
                // Check if it's a word boundary
                bool isWordStart = (pos == 0) || !std::isalnum(exprText[pos-1]);
                bool isWordEnd = (pos + varName.length() >= exprText.length()) || 
                                !std::isalnum(exprText[pos + varName.length()]);
                
                if (isWordStart && isWordEnd) {
                    std::cout << "        DEBUG: Replacing '" << varName << "' with '" << replacement << "' at position " << pos << "\n";
                    exprText.replace(pos, varName.length(), replacement);
                    pos += replacement.length();
                } else {
                    pos += varName.length();
                }
            }
        }
        
        if (originalText != exprText) {
            std::cout << "        DEBUG: rewriteExpression output: '" << exprText << "'\n";
        } else {
            std::cout << "        DEBUG: No changes made to expression\n";
        }
        
        return exprText;
    }
    
    std::string rewriteExpressionExceptVar(const Expr* expr, const std::string& excludeVar) {
        if (!expr) return "";
        
        SourceRange range = expr->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(range);
        std::string exprText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
        
        std::cout << "        DEBUG: rewriteExpressionExceptVar input: '" << exprText << "'\n";
        std::cout << "        DEBUG: Expression type: " << expr->getStmtClassName() << "\n";
        std::cout << "        DEBUG: Excluding variable: '" << excludeVar << "'\n";
        
        return rewriteVariableReferencesInText(exprText, excludeVar);
    }

    void insertDestructorsForScope(const ScopeInfo& scope) {
        std::cout << "    DEBUG: Inserting destructors for scope with " << scope.variablesInScope.size() << " variables\n";
        
        if (scope.variablesInScope.empty()) {
            return;
        }
        
        // Insert destructors in reverse order (LIFO - last constructed, first destroyed)
        std::string destructorCalls;
        for (auto it = scope.variablesInScope.rbegin(); it != scope.variablesInScope.rend(); ++it) {
            const std::string& varName = *it;
            std::cout << "      DEBUG: Adding destructor call for variable: " << varName << "\n";
            destructorCalls += "    this->state." + varName + ".destroy();\n";
        }
        
        if (!destructorCalls.empty()) {
            // Insert before the closing brace of the scope
            SourceLocation insertLoc = scope.scopeEnd;
            destructorInsertions.emplace_back(insertLoc, destructorCalls);
            std::cout << "    DEBUG: Scheduled destructor insertions before scope end\n";
        }
    }

    bool isPartOfDeclaration(const DeclRefExpr* declRef) {
        // Check if this reference is part of a declaration statement we're processing
        const Stmt* parent = declRef;
        while (parent) {
            if (auto* declStmt = dyn_cast<DeclStmt>(parent)) {
                for (auto* decl : declStmt->decls()) {
                    if (auto* varDecl = dyn_cast<VarDecl>(decl)) {
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
        std::cout << "  DEBUG: Collecting coroutine statement replacements for " << coroutineStatements.size() << " statements\n";
        
        for (const auto& coroStmt : coroutineStatements) {
            if (coroStmt.type == CoroutineStatement::YIELD) {
                std::cout << "    DEBUG: Collecting co_yield replacement for CO_YIELD(" << coroStmt.index << ", ...)\n";
                
                // Replace "co_yield" with "CO_YIELD(index, "
                std::string macroStart = "CO_YIELD(" + std::to_string(coroStmt.index) + ", ";
                
                // Find the end of "co_yield" keyword
                SourceLocation keywordEnd = Lexer::getLocForEndOfToken(coroStmt.keywordLoc, 0, sourceManager, LangOptions());
                
                // Add to global replacements
                globalReplacements.emplace_back(coroStmt.keywordLoc, macroStart);
                
                // Add closing parenthesis after the operand
                if (coroStmt.operand) {
                    SourceLocation operandEnd = Lexer::getLocForEndOfToken(coroStmt.operandEnd, 0, sourceManager, LangOptions());
                    globalReplacements.emplace_back(operandEnd, ")");
                }
                
            } else if (coroStmt.type == CoroutineStatement::AWAIT) {
                std::cout << "    DEBUG: Collecting co_await replacement for CO_AWAIT(" << coroStmt.index << ", ...)\n";
                
                // Replace "co_await" with "CO_AWAIT(index, "
                std::string macroStart = "CO_AWAIT(" + std::to_string(coroStmt.index) + ", ";
                
                // Find the end of "co_await" keyword  
                SourceLocation keywordEnd = Lexer::getLocForEndOfToken(coroStmt.keywordLoc, 0, sourceManager, LangOptions());
                
                // Add to global replacements
                globalReplacements.emplace_back(coroStmt.keywordLoc, macroStart);
                
                // Add closing parenthesis after the operand
                if (coroStmt.operand) {
                    SourceLocation operandEnd = Lexer::getLocForEndOfToken(coroStmt.operandEnd, 0, sourceManager, LangOptions());
                    globalReplacements.emplace_back(operandEnd, ")");
                }
            }
        }
        
        std::cout << "  Collected " << coroutineStatements.size() << " coroutine statement replacements into global vector\n";
    }

private:
    void collectRangedForFooterInsertions() {
        std::cout << "  DEBUG: Collecting ranged-for footer insertions into global vector\n";
        
        for (const auto& rangedFor : rangedForLoops) {
            // Find the location where FOR_LOOP_FOOTER should be inserted
            // This should be after the closing brace of the for loop, but before the closing brace of the outer scope
            const Stmt* body = rangedFor.stmt->getBody();
            if (body) {
                if (auto* compoundBody = dyn_cast<CompoundStmt>(body)) {
                    // Calculate where the footer should go in the transformed code
                    // In the explicit form, the footer goes after "  }" (the for loop close) but before "}" (scope close)
                    SourceLocation footerLoc = compoundBody->getRBracLoc();
                    std::string footerMacro = "  FOR_LOOP_FOOTER(" + std::to_string(rangedFor.index) + ")\n";
                    globalReplacements.emplace_back(footerLoc, footerMacro);
                    std::cout << "    Added FOR_LOOP_FOOTER(" << rangedFor.index << ") insertion at " 
                              << footerLoc.printToString(sourceManager) << "\n";
                }
            }
        }
        
        std::cout << "  Collected " << rangedForLoops.size() << " ranged-for footer insertions into global vector\n";
    }

public:
    void collectRangedForLoopReplacements() {
        std::cout << "  DEBUG: Collecting ranged-for loop bulk replacements for " << rangedForLoops.size() << " loops\n";
        
        // First collect footer insertions into global vector
        collectRangedForFooterInsertions();
        
        // Ranged-for loop replacements are range-based (SourceRange), not insertion-based (SourceLocation)
        // So we need to apply them immediately, similar to other range replacements
        // Sort ranged-for loops by location in reverse order to avoid invalidating positions
        std::sort(rangedForLoops.begin(), rangedForLoops.end(),
                 [&](const RangedForLoop& a, const RangedForLoop& b) {
                     return sourceManager.getFileOffset(a.fullRange.getBegin()) > sourceManager.getFileOffset(b.fullRange.getBegin());
                 });
        
        for (const auto& rangedFor : rangedForLoops) {
            std::cout << "\n=== APPLYING RANGED-FOR BULK REPLACEMENT ===\n";
            std::cout << "    DEBUG: Bulk replacing ranged-for loop " << rangedFor.index << "\n";
            std::cout << "    Replacement target range: " << rangedFor.fullRange.getBegin().printToString(sourceManager) 
                      << " to " << rangedFor.fullRange.getEnd().printToString(sourceManager) << "\n";
            
            // Get original text for comparison
            CharSourceRange originalRange = CharSourceRange::getTokenRange(rangedFor.fullRange);
            std::string originalText = Lexer::getSourceText(originalRange, sourceManager, LangOptions()).str();
            std::cout << "    Original text being replaced: '" << originalText << "'\n";
            std::cout << "    Original text length: " << originalText.length() << " characters\n";
            
            // Generate the explicit loop form (includes FOR_LOOP_HEADER but NOT FOR_LOOP_FOOTER)
            std::string explicitLoop = generateExplicitLoopForm(rangedFor);
            globalReplacements.emplace_back(rangedFor.fullRange, explicitLoop);

            /*
            // Replace the entire ranged-for statement
            std::cout << "    About to call rewriter.ReplaceText(...)\n";
            rewriter.ReplaceText(rangedFor.fullRange, explicitLoop);
            std::cout << "    SUCCESS: Bulk replaced ranged-for with explicit form\n";
            std::cout << "=== END APPLYING RANGED-FOR BULK REPLACEMENT ===\n\n";
            */
        }
        
        std::cout << "  Applied " << rangedForLoops.size() << " ranged-for loop bulk replacements\n";
    }
    
    
    std::string generateExplicitLoopForm(const RangedForLoop& rangedFor) {
        std::cout << "\n=== GENERATING EXPLICIT LOOP FORM ===\n";
        std::cout << "      DEBUG: Generating explicit loop form for loop " << rangedFor.index << "\n";
        std::cout << "      Original ranged-for range: " << rangedFor.fullRange.getBegin().printToString(sourceManager) 
                  << " to " << rangedFor.fullRange.getEnd().printToString(sourceManager) << "\n";
        
        // Get the loop body - keep it as-is for now, it will be processed by normal coroutine rewriting
        std::string loopBody = "";
        const Stmt* body = rangedFor.stmt->getBody();
        if (body) {
            SourceRange bodyRange = body->getSourceRange();
            CharSourceRange charRange = CharSourceRange::getTokenRange(bodyRange);
            loopBody = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();
            
            std::cout << "      Original loop body range: " << bodyRange.getBegin().printToString(sourceManager) 
                      << " to " << bodyRange.getEnd().printToString(sourceManager) << "\n";
            std::cout << "      Original loop body text: '" << loopBody << "'\n";
            std::cout << "      Original loop body length: " << loopBody.length() << " characters\n";
        } else {
            std::cout << "      ERROR: No loop body found!\n";
        }
        
        std::string result = "{\n";
        std::cout << "      Building replacement text:\n";
        
        // Use external FOR_LOOP_HEADER macro for initialization
        std::string headerMacro = "  FOR_LOOP_HEADER(" + std::to_string(rangedFor.index) + ")\n";
        result += headerMacro;
        std::cout << "        1. Added header: '" << headerMacro.substr(0, headerMacro.length()-1) << "'\n";
        
        // for (; this->state.__begin.get() != this->state.__end.get(); ++this->state.__begin.get()) {
        std::string forLine = "  for (; this->state." + rangedFor.beginVarName + ".get() != this->state." + rangedFor.endVarName + ".get(); ++this->state." + rangedFor.beginVarName + ".get()) {\n";
        result += forLine;
        std::cout << "        2. Added for line: '" << forLine.substr(0, forLine.length()-1) << "'\n";
        
        // range_declaration = *this->state.__begin.get();
        std::string loopVarDecl = "    " + rangedFor.loopVarType + " " + rangedFor.loopVarName + " = *this->state." + rangedFor.beginVarName + ".get();\n";
        result += loopVarDecl;
        std::cout << "        3. Added loop var decl: '" << loopVarDecl.substr(0, loopVarDecl.length()-1) << "'\n";
        
        // Insert the loop body (without the outer braces if it's a compound statement)
        std::cout << "        4. Processing loop body...\n";
        if (loopBody.size() >= 2 && loopBody.front() == '{' && loopBody.back() == '}') {
            // Remove outer braces and add the inner content with proper indentation
            std::string innerBody = loopBody.substr(1, loopBody.size() - 2);
            std::cout << "           Detected compound body, extracting inner: '" << innerBody << "'\n";
            result += "    " + innerBody + "\n";
        } else {
            std::cout << "           Using body as-is: '" << loopBody << "'\n";
            result += "    " + loopBody + "\n";
        }
        
        // Close the for loop - NOTE: FOR_LOOP_FOOTER will be added via global replacements
        std::string forClose = "  }\n"; // Close the for loop
        result += forClose;
        std::cout << "        5. Added for close: '" << forClose.substr(0, forClose.length()-1) << "'\n";
        
        std::string scopeClose = "}"; // Close the outer scope
        result += scopeClose;
        std::cout << "        6. Added scope close: '" << scopeClose << "'\n";
        
        std::cout << "      FINAL Generated explicit loop text (length=" << result.length() << "):\n";
        std::cout << "=== START GENERATED TEXT ===\n" << result << "\n=== END GENERATED TEXT ===\n";
        std::cout << "=== END GENERATING EXPLICIT LOOP FORM ===\n\n";
        
        return result;
    }

public:
    void applyReplacements() {
        // Collect all insertion-style replacements into global vector
        collectCoroutineStatementReplacements();
        collectDestructorInsertions();
        // Note: ranged-for footer insertions will be collected in collectRangedForLoopReplacements
        
        // Apply range-style replacements first (declarations and references)  
        collectRangeReplacements();
        
        // Apply ranged-for loop bulk replacements (this also collects footer insertions)
        collectRangedForLoopReplacements();
        
        // Finally apply all insertion-style replacements with global sorting
        applyAllReplacements();
    }
    
    void collectDestructorInsertions() {
        std::cout << "  DEBUG: Collecting destructor insertions into global vector\n";
        for (const auto& insertion : destructorInsertions) {
            globalReplacements.emplace_back(insertion.first, insertion.second);
        }
        std::cout << "  Collected " << destructorInsertions.size() << " destructor insertions into global vector\n";
    }
    
    void collectRangeReplacements() {
        std::cout << "  DEBUG: Collecting range-style replacements (declarations and references) into global vector\n";
        
        // Combine all range replacements (declarations and references)
        globalReplacements.insert(globalReplacements.end(), declReplacements.begin(), declReplacements.end());
        globalReplacements.insert(globalReplacements.end(), refReplacements.begin(), refReplacements.end());

        /*
        // Convert SourceRange replacements to multiple SourceLocation insertions if needed
        // For now, we'll apply range replacements immediately since they can't be easily converted to insertions
        // Sort by source location in reverse order to avoid invalidating positions
        std::sort(allRangeReplacements.begin(), allRangeReplacements.end(), 
                 [&](const std::pair<SourceRange, std::string>& a, const std::pair<SourceRange, std::string>& b) {
                     return sourceManager.getFileOffset(a.first.getBegin()) > sourceManager.getFileOffset(b.first.getBegin());
                 });

        for (const auto& replacement : allRangeReplacements) {
            std::cout << "  Applying range replacement: " << replacement.second << "\n";
            rewriter.ReplaceText(replacement.first, replacement.second);
        }
        
        std::cout << "  Applied " << allRangeReplacements.size() << " range-style replacements\n";
        */
    }
    
    void applyAllReplacements() {
        std::cout << "\n=== APPLYING ALL INSERTION-STYLE REPLACEMENTS ===\n";
        std::cout << "  DEBUG: Sorting and applying " << globalReplacements.size() << " insertion-style replacements\n";
        
        // Sort ALL insertion-style replacements by position in reverse order
        std::stable_sort(globalReplacements.begin(), globalReplacements.end(),
                 [&](const std::pair<SourceRange, std::string>& a, const std::pair<SourceRange, std::string>& b) {
                     unsigned offsetA = sourceManager.getFileOffset(a.first.getBegin());
                     unsigned offsetB = sourceManager.getFileOffset(b.first.getBegin());
                     if (offsetA == offsetB) {
                         // Same position - FOR_LOOP_FOOTER comes first (as requested)
                         bool aIsFooter = a.second.find("FOR_LOOP_FOOTER") != std::string::npos;
                         bool bIsFooter = b.second.find("FOR_LOOP_FOOTER") != std::string::npos;
                         if (aIsFooter && !bIsFooter) return true;
                         if (!aIsFooter && bIsFooter) return false;
                         // If both or neither are footers, maintain stable order
                         return false;
                     }
                     return offsetA > offsetB; // Reverse order for position safety
                 });
        
        // Apply all replacements in the determined order
        // TODO<joka921> We have to get a priority to the global replacements.
        std::optional<std::pair<SourceRange, std::string>> buffer;
        for (size_t i = 0; i < globalReplacements.size(); ++i) {
            auto& replacement = globalReplacements[i];
            if (!buffer.has_value()) {
                buffer.emplace(std::move(replacement));
                continue;
            }
            if (buffer->first != globalReplacements[i].first) {
                std::cout << "Inserting at " << buffer->first.printToString(sourceManager)
                          << ": '" << buffer->second << "'\n";
                rewriter.ReplaceText(buffer->first, buffer->second);
                buffer = std::move(replacement);
                continue;
            }
            buffer->second += replacement.second;
        }
        if (buffer.has_value()) {
            std::cout << "Inserting at " << buffer->first.printToString(sourceManager)
                      << ": '" << buffer->second << "'\n";
            rewriter.ReplaceText(buffer->first, buffer->second);
        }
        
        std::cout << "  Applied " << globalReplacements.size() << " insertion-style replacements with global sorting\n";
        std::cout << "=== END APPLYING ALL INSERTION-STYLE REPLACEMENTS ===\n\n";
    }
    
    // Getter methods for accessing replacements without applying them
    const std::vector<std::pair<SourceRange, std::string>>& getDeclReplacements() const {
        return declReplacements;
    }
    
    const std::vector<std::pair<SourceRange, std::string>>& getRefReplacements() const {
        return refReplacements;
    }
    
    const std::vector<std::pair<SourceLocation, std::string>>& getDestructorInsertions() const {
        return destructorInsertions;
    }
    
    const std::vector<RangedForLoop>& getRangedForLoops() const {
        return rangedForLoops;
    }
};

class CoroutineRewriter : public RecursiveASTVisitor<CoroutineRewriter> {
private:
    const SourceManager& sourceManager;
    Rewriter& rewriter;
    clang::DiagnosticsEngine& diagnosticsEngine;
    const LangOptions& langOptions;
    ASTContext* astContext;
    std::vector<CoroutineInfo> coroutines;

    bool containsCoroutineKeywords(const Stmt* stmt) {
        if (!stmt) return false;
        
        if (isa<CoawaitExpr>(stmt) || isa<CoyieldExpr>(stmt) || isa<CoreturnStmt>(stmt)) {
            return true;
        }
        
        for (auto it = stmt->child_begin(); it != stmt->child_end(); ++it) {
            if (const Stmt* child = *it) {
                if (containsCoroutineKeywords(child)) {
                    return true;
                }
            }
        }
        return false;
    }

    void collectLocalVariables(const Stmt* body, std::set<LocalVariable>& variables) {
        LocalVariableCollector collector(variables, sourceManager, *astContext);
        collector.TraverseStmt(const_cast<Stmt*>(body));
    }

    std::string replaceLastTemplateArgWithHandle(const std::string& returnType) {
        std::cout << "    DEBUG: replaceLastTemplateArgWithHandle input: '" << returnType << "'\n";
        
        // Find the template arguments by looking for < and >
        size_t openAngle = returnType.find('<');
        if (openAngle == std::string::npos) {
            std::cout << "    DEBUG: No template arguments found, returning unchanged\n";
            return returnType;
        }
        
        // Find the matching closing angle bracket
        size_t closeAngle = returnType.rfind('>');
        if (closeAngle == std::string::npos || closeAngle <= openAngle) {
            std::cout << "    DEBUG: Invalid template syntax, returning unchanged\n";
            return returnType;
        }
        
        // Extract the template arguments part
        std::string templateArgs = returnType.substr(openAngle + 1, closeAngle - openAngle - 1);
        std::cout << "    DEBUG: Template arguments: '" << templateArgs << "'\n";
        
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
        
        std::cout << "    DEBUG: Parsed " << args.size() << " template arguments:\n";
        for (size_t i = 0; i < args.size(); ++i) {
            std::cout << "      [" << i << "]: '" << args[i] << "'\n";
        }
        
        if (args.empty()) {
            std::cout << "    DEBUG: No template arguments found after parsing\n";
            return returnType;
        }
        
        // Replace the last argument with Handle
        std::cout << "    DEBUG: Replacing last argument '" << args.back() << "' with 'Handle'\n";
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
        std::cout << "    DEBUG: replaceLastTemplateArgWithHandle output: '" << result << "'\n";
        return result;
    }

    SourceLocation findStructInsertionPoint(const FunctionDecl* funcDecl) {
        std::cout << "DEBUG: findStructInsertionPoint called for function: " << funcDecl->getQualifiedNameAsString() << "\n";
        
        const Stmt* bodyStmt = funcDecl->getBody();
        if (!bodyStmt) {
            std::cout << "DEBUG: Function has no body\n";
            return SourceLocation();
        }
        
        std::cout << "DEBUG: Function has body, type: " << bodyStmt->getStmtClassName() << "\n";
        
        // Handle CoroutineBodyStmt wrapper
        if (auto* coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            std::cout << "DEBUG: Body is CoroutineBodyStmt, getting inner body\n";
            bodyStmt = coroBody->getBody();
            if (!bodyStmt) {
                std::cout << "DEBUG: CoroutineBodyStmt has no inner body\n";
                return SourceLocation();
            }
            std::cout << "DEBUG: Inner body type: " << bodyStmt->getStmtClassName() << "\n";
        }
        
        if (auto* body = dyn_cast<CompoundStmt>(bodyStmt)) {
            std::cout << "DEBUG: Body is CompoundStmt\n";
            SourceLocation lbracLoc = body->getLBracLoc();
            std::cout << "DEBUG: getLBracLoc() returned location, checking validity\n";
            
            if (lbracLoc.isValid()) {
                std::cout << "DEBUG: lbracLoc is valid, calling Lexer::getLocForEndOfToken\n";
                SourceLocation endLoc = Lexer::getLocForEndOfToken(lbracLoc, 0, sourceManager, langOptions);
                if (endLoc.isValid()) {
                    std::cout << "DEBUG: Successfully found insertion point\n";
                    return endLoc;
                } else {
                    std::cout << "DEBUG: Lexer::getLocForEndOfToken returned invalid location\n";
                }
            } else {
                std::cout << "DEBUG: lbracLoc is invalid\n";
            }
        } else {
            std::cout << "DEBUG: Final body is not CompoundStmt, type: " << bodyStmt->getStmtClassName() << "\n";
        }
        
        std::cout << "DEBUG: Returning invalid SourceLocation\n";
        return SourceLocation();
    }

    std::string generateCoroImplStruct(const CoroutineInfo& coro) {
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
            std::cout << "  DEBUG: Original canonical return type: " << originalReturnType << "\n";
            
            // Also print the non-canonical version for comparison
            std::string nonCanonicalType = retType.getAsString(policy);
            std::cout << "  DEBUG: Non-canonical return type: " << nonCanonicalType << "\n";
            
            // Replace last template argument with HANDLE (work on canonical type)
            returnType = replaceLastTemplateArgWithHandle(originalReturnType);
            std::cout << "  DEBUG: Modified return type: " << returnType << "\n";
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
        std::cout << "  DEBUG: generateCoroImplStruct - Processing " << coro.localVariables.size() << " variables:\n";
        for (const auto& var : coro.localVariables) {
            std::cout << "    Variable: " << var.type << " " << var.name << "\n";
        }
        
        // Add all local variables (including ranged-for variables)
        if (coro.localVariables.empty()) {
            structCode += "    // No local variables found in this coroutine\n";
        } else {
            structCode += "    // Local variables (including ranged-for loop variables)\n";
            for (const auto& var : coro.localVariables) {
                structCode += "    _coro_storage<" + var.type + "> " + var.name + ";\n";
                std::cout << "  DEBUG: Added to struct: _coro_storage<" << var.type << "> " << var.name << ";\n";
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
    CoroutineRewriter(Rewriter& rewr, const SourceManager& SM, DiagnosticsEngine& diag, const LangOptions& langOpts)
        : sourceManager(SM), rewriter(rewr), diagnosticsEngine(diag), langOptions(langOpts), astContext(nullptr) {}

    void setASTContext(ASTContext& ctx) {
        astContext = &ctx;
    }

    bool VisitFunctionDecl(FunctionDecl* funcDecl) {
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
            
            std::cout << "Processing coroutine: " << funcDecl->getQualifiedNameAsString() << "\n";
            
            collectLocalVariables(funcDecl->getBody(), coro.localVariables);
            
            coro.insertionPoint = findStructInsertionPoint(funcDecl);
            if (coro.insertionPoint.isInvalid()) {
                unsigned diagID = diagnosticsEngine.getCustomDiagID(
                    clang::DiagnosticsEngine::Warning,
                    "Could not find insertion point for _detail_coro_impl struct");
                diagnosticsEngine.Report(funcDecl->getLocation(), diagID);
                coro.hasError = true;
                
                // Output the struct code to console as fallback
                std::string structCode = generateCoroImplStruct(coro);
                std::cout << "WARNING: Could not insert struct, here's what would have been inserted:\n";
                std::cout << "========== STRUCT CODE ==========\n";
                std::cout << structCode;
                std::cout << "==================================\n";
            }
            
            coroutines.push_back(coro);
            
            std::cout << "  Found " << coro.localVariables.size() << " local variables\n";
            
            if (const auto* methodDecl = dyn_cast<CXXMethodDecl>(funcDecl)) {
                std::cout << "  Type: Member function of " << methodDecl->getParent()->getQualifiedNameAsString() << "\n";
            } else {
                std::cout << "  Type: Free function\n";
            }
        }

        return true;
    }

    void updateFunctionReturnType(const CoroutineInfo& coro) {
        std::cout << "Updating function return type for: " << coro.function->getQualifiedNameAsString() << "\n";
        
        if (!coro.function) {
            std::cout << "  ERROR: No function to update\n";
            return;
        }
        
        // Get the return type location
        SourceRange returnTypeRange = coro.function->getReturnTypeSourceRange();
        if (returnTypeRange.isInvalid()) {
            std::cout << "  ERROR: Invalid return type source range\n";
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
        
        std::cout << "  DEBUG: Original canonical return type: " << originalReturnType << "\n";
        std::cout << "  DEBUG: Modified return type: " << modifiedReturnType << "\n";
        
        if (originalReturnType != modifiedReturnType) {
            rewriter.ReplaceText(returnTypeRange, modifiedReturnType);
            std::cout << "  Updated function return type\n";
        } else {
            std::cout << "  Return type unchanged (no template arguments found)\n";
        }
    }

    void performRewrites() {
        for (const auto& coro : coroutines) {
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
                std::cout << "Inserted _detail_coro_impl struct into " 
                         << coro.function->getQualifiedNameAsString() << "\n";
            }
        }
    }

    void rewriteCoroutineBody(const CoroutineInfo& coro) {
        std::cout << "Rewriting coroutine body for: " << coro.function->getQualifiedNameAsString() << "\n";
        
        const Stmt* bodyStmt = coro.function->getBody();
        if (!bodyStmt) {
            return;
        }
        
        // Handle CoroutineBodyStmt wrapper
        if (auto* coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            bodyStmt = coroBody->getBody();
        }
        
        if (bodyStmt) {
            // First pass: collect ranged-for loops only (don't apply any replacements yet)
            CoroutineBodyRewriter initialRewriter(coro.localVariables, rewriter, sourceManager);
            initialRewriter.TraverseStmt(const_cast<Stmt*>(bodyStmt));
            
            // Get the ranged-for loops and add them to the coroutine info for struct generation
            const auto& rangedForLoops = initialRewriter.getRangedForLoops();
            const_cast<CoroutineInfo&>(coro).rangedForLoops = rangedForLoops;
            std::cout << "  DEBUG: Found " << rangedForLoops.size() << " ranged-for loops\n";
            
            // Add ranged-for variables to the local variables set so they appear in the struct
            for (const auto& rangedFor : rangedForLoops) {
                LocalVariable rangeVar;
                rangeVar.name = rangedFor.rangeVarName;
                rangeVar.type = "decltype(" + rangedFor.rangeExpr + ")";
                rangeVar.location = rangedFor.fullRange.getBegin();
                const_cast<CoroutineInfo&>(coro).localVariables.insert(rangeVar);
                
                LocalVariable beginVar;
                beginVar.name = rangedFor.beginVarName;
                beginVar.type = "decltype(begin(std::declval<decltype(" + rangedFor.rangeExpr + ")>()))";
                beginVar.location = rangedFor.fullRange.getBegin();
                const_cast<CoroutineInfo&>(coro).localVariables.insert(beginVar);
                
                LocalVariable endVar;
                endVar.name = rangedFor.endVarName;
                endVar.type = "decltype(end(std::declval<decltype(" + rangedFor.rangeExpr + ")>()))";
                endVar.location = rangedFor.fullRange.getBegin();
                const_cast<CoroutineInfo&>(coro).localVariables.insert(endVar);
                
                std::cout << "    Added ranged-for variables to struct: " << rangedFor.rangeVarName 
                         << ", " << rangedFor.beginVarName << ", " << rangedFor.endVarName << "\n";
            }
            
            // Apply only ranged-for loop replacements first (this transforms ranged-for to explicit loops)
            //std::cout << "  DEBUG: Applying ranged-for loop transformations\n";
            //initialRewriter.applyRangedForLoopReplacements();
            
            // Second pass: create new rewriter with all variables (including ranged-for vars)
            // This will process the explicit loops with proper variable rewriting
            std::cout << "  DEBUG: Creating final rewriter with " << coro.localVariables.size() << " variables:\n";
            for (const auto& var : coro.localVariables) {
                std::cout << "    Variable: " << var.type << " " << var.name << "\n";
            }
            
            CoroutineBodyRewriter finalRewriter(coro.localVariables, rewriter, sourceManager);
            finalRewriter.TraverseStmt(const_cast<Stmt*>(bodyStmt));
            
            // Apply all the variable rewrites in place (construct/destroy calls, get() access)  
            std::cout << "  DEBUG: Applying all variable transformations in place\n";

            // Now wrap the transformed code with run() method braces
            std::cout << "  DEBUG: Wrapping transformed code with run() method\n";
            wrapBodyWithRunMethod(coro, finalRewriter);
            finalRewriter.applyReplacements();

            std::cout << "Completed body rewriting for: " << coro.function->getQualifiedNameAsString() << "\n";
        }
    }
    
    std::string getTransformedBodyText(const Stmt* bodyStmt, CoroutineBodyRewriter& bodyRewriter) {
        // Get the compound statement body
        if (auto* compoundStmt = dyn_cast<CompoundStmt>(bodyStmt)) {
            // Get the inner part (without the outer braces)
            SourceLocation startLoc = Lexer::getLocForEndOfToken(compoundStmt->getLBracLoc(), 0, sourceManager, langOptions);
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
    
    std::string applyTransformationsInMemory(const std::string& originalText, CoroutineBodyRewriter& bodyRewriter) {
        std::string result = originalText;
        
        // Get all the replacements that would have been made
        const auto& declReplacements = bodyRewriter.getDeclReplacements();
        const auto& refReplacements = bodyRewriter.getRefReplacements();
        const auto& destructorInsertions = bodyRewriter.getDestructorInsertions();
        
        // For now, we'll do simple text-based transformations
        // In a full implementation, we would need more sophisticated source location mapping
        
        std::cout << "    DEBUG: Original text length: " << originalText.length() << "\n";
        std::cout << "    DEBUG: Found " << declReplacements.size() << " declaration replacements\n";
        std::cout << "    DEBUG: Found " << refReplacements.size() << " reference replacements\n";
        std::cout << "    DEBUG: Found " << destructorInsertions.size() << " destructor insertions\n";
        
        // Simple regex-based replacement for now
        // TODO: Implement proper source location mapping for precise replacements
        
        return result;
    }
    
    void replaceEntireBodyWithStateMachine(const CoroutineInfo& coro) {
        std::cout << "  DEBUG: Replacing entire body with state machine instantiation\n";
        
        const Stmt* originalBody = coro.function->getBody();
        if (auto* coroBody = dyn_cast<CoroutineBodyStmt>(originalBody)) {
            originalBody = coroBody->getBody();
        }
        
        if (auto* compoundStmt = dyn_cast<CompoundStmt>(originalBody)) {
            SourceLocation lbraceLoc = compoundStmt->getLBracLoc();
            SourceLocation rbraceLoc = compoundStmt->getRBracLoc();
            
            std::cout << "  DEBUG: LBrace location: " << lbraceLoc.printToString(sourceManager) << "\n";
            std::cout << "  DEBUG: RBrace location: " << rbraceLoc.printToString(sourceManager) << "\n";
            
            // Validate basic locations
            if (lbraceLoc.isInvalid() || rbraceLoc.isInvalid()) {
                std::cout << "  ERROR: Invalid brace locations\n";
                return;
            }
            
            // Get the full range including braces
            SourceRange fullRange = compoundStmt->getSourceRange();
            std::cout << "  DEBUG: Full compound statement range: " 
                      << fullRange.getBegin().printToString(sourceManager) << " to "
                      << fullRange.getEnd().printToString(sourceManager) << "\n";
            
            // Get the original text to see what we're working with
            CharSourceRange charRange = CharSourceRange::getCharRange(fullRange);
            std::string originalText = Lexer::getSourceText(charRange, sourceManager, langOptions).str();
            std::cout << "  DEBUG: Original compound statement text: '" << originalText << "'\n";
            std::cout << "  DEBUG: Original text length: " << originalText.length() << "\n";
            
            // Calculate file offsets for debugging
            unsigned startOffset = sourceManager.getFileOffset(fullRange.getBegin());
            unsigned endOffset = sourceManager.getFileOffset(fullRange.getEnd());
            std::cout << "  DEBUG: File offset range: " << startOffset << " to " << endOffset 
                      << " (length: " << (endOffset - startOffset + 1) << ")\n";
            
            // Try a safer approach: replace the entire compound statement
            std::string stateMachineCall = "{\n    _detail_coro_statemachine_impl stateMachine;\n    stateMachine.run();\n  }";
            
            std::cout << "  DEBUG: About to replace with: '" << stateMachineCall << "'\n";
            std::cout << "  DEBUG: Replacement length: " << stateMachineCall.length() << "\n";
            
            // Use the full range instead of trying to calculate inner range
                rewriter.ReplaceText(fullRange, stateMachineCall);
            /*
                try {
                rewriter.ReplaceText(fullRange, stateMachineCall);
                std::cout << "  DEBUG: Successfully replaced compound statement\n";
            } catch (...) {
                std::cout << "  ERROR: Exception during replacement\n";
            }
            */
        }
    }
    

    void wrapBodyWithRunMethod(const CoroutineInfo& coro, CoroutineBodyRewriter& body_rewriter) {
        std::cout << "  DEBUG: Adding closing braces after coroutine body for: " << coro.function->getQualifiedNameAsString() << "\n";
        
        const Stmt* bodyStmt = coro.function->getBody();
        if (!bodyStmt) {
            std::cout << "  ERROR: Function has no body to wrap\n";
            return;
        }
        
        // Handle CoroutineBodyStmt wrapper
        if (auto* coroBody = dyn_cast<CoroutineBodyStmt>(bodyStmt)) {
            bodyStmt = coroBody->getBody();
        }
        
        if (auto* compoundStmt = dyn_cast<CompoundStmt>(bodyStmt)) {
            SourceLocation rbraceLoc = compoundStmt->getRBracLoc();
            
            std::cout << "  DEBUG: Found compound statement closing brace\n";
            std::cout << "  DEBUG: RBrace location: " << rbraceLoc.printToString(sourceManager) << "\n";
            
            if (rbraceLoc.isValid()) {
                // Replace exactly one character (the closing brace) with COROUTINE_FOOTER + closing brace
                std::string coroutineFooterAndBrace = "COROUTINE_FOOTER\n}";
                body_rewriter.globalReplacements.push_back(std::make_pair(rbraceLoc, coroutineFooterAndBrace));
                //rewriter.ReplaceText(rbraceLoc, 1, coroutineFooterAndBrace);
                std::cout << "  DEBUG: Replaced closing brace with COROUTINE_FOOTER + brace\n";
                
                std::cout << "  DEBUG: Successfully added closing braces\n";
            } else {
                std::cout << "  ERROR: Invalid closing brace location for compound statement\n";
            }
        } else {
            std::cout << "  ERROR: Body is not a compound statement, cannot add closing braces\n";
        }
    }

    const std::vector<CoroutineInfo>& getCoroutines() const {
        return coroutines;
    }
};

class MyASTConsumer : public ASTConsumer {
private:
    CoroutineRewriter& rewriter;

public:
    MyASTConsumer(CoroutineRewriter& rewr) : rewriter(rewr) {}

    void HandleTranslationUnit(ASTContext& Context) override {
        rewriter.setASTContext(Context);
        rewriter.TraverseDecl(Context.getTranslationUnitDecl());
    }
};

class CoroutineRewriterFrontendAction : public ASTFrontendAction {
private:
    std::unique_ptr<Rewriter> rewriter;
    SourceManager* sourceManager;
    std::unique_ptr<CoroutineRewriter> coroutineRewriter;

public:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& CI, llvm::StringRef InFile) override {
        
        sourceManager = &CI.getSourceManager();
        rewriter = std::make_unique<Rewriter>();
        rewriter->setSourceMgr(*sourceManager, CI.getLangOpts());
        
        coroutineRewriter = std::make_unique<CoroutineRewriter>(
            *rewriter, *sourceManager, CI.getDiagnostics(), CI.getLangOpts());
        
        return std::make_unique<MyASTConsumer>(*coroutineRewriter);
    }

    void EndSourceFileAction() override {
        coroutineRewriter->performRewrites();
        
        const auto& coroutines = coroutineRewriter->getCoroutines();
        std::cout << "\nSummary: Processed " << coroutines.size() << " coroutine(s)\n";
        
        SourceLocation mainFileLoc = sourceManager->getLocForStartOfFile(sourceManager->getMainFileID());
        const std::string filePath = sourceManager->getFilename(mainFileLoc).str();
        
        if (rewriter->getRewriteBufferFor(sourceManager->getMainFileID())) {
            const RewriteBuffer& RewriteBuf = rewriter->getEditBuffer(sourceManager->getMainFileID());
            
            std::error_code EC;
            llvm::raw_fd_ostream OS(filePath, EC, llvm::sys::fs::OF_Text);
            if (EC) {
                llvm::errs() << "Error opening file for writing: " << EC.message() << "\n";
                return;
            }
            
            RewriteBuf.write(OS);
            std::cout << "Rewrote file: " << filePath << "\n";
        } else {
            std::cout << "No changes needed for: " << filePath << "\n";
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