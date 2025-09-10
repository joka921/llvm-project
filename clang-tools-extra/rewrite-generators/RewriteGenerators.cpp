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

struct CoroutineInfo {
    const FunctionDecl* function;
    std::set<LocalVariable> localVariables;
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

public:
    CoroutineBodyRewriter(const std::set<LocalVariable>& vars, Rewriter& rewr, const SourceManager& SM)
        : localVariables(vars), rewriter(rewr), sourceManager(SM), nextCoroStatementIndex(0) {
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

    void applyCoroutineStatementReplacements() {
        std::cout << "  DEBUG: Applying coroutine statement replacements for " << coroutineStatements.size() << " statements\n";
        
        // Sort coroutine statements by location in reverse order to avoid invalidating positions  
        std::sort(coroutineStatements.begin(), coroutineStatements.end(),
                 [&](const CoroutineStatement& a, const CoroutineStatement& b) {
                     return sourceManager.getFileOffset(a.keywordLoc) > sourceManager.getFileOffset(b.keywordLoc);
                 });
        
        for (const auto& coroStmt : coroutineStatements) {
            if (coroStmt.type == CoroutineStatement::YIELD) {
                std::cout << "    DEBUG: Replacing co_yield with CO_YIELD(" << coroStmt.index << ", ...)\n";
                
                // Replace "co_yield" with "CO_YIELD(index, "
                std::string macroStart = "CO_YIELD(" + std::to_string(coroStmt.index) + ", ";
                
                // Find the end of "co_yield" keyword
                SourceLocation keywordEnd = Lexer::getLocForEndOfToken(coroStmt.keywordLoc, 0, sourceManager, LangOptions());
                SourceRange keywordRange(coroStmt.keywordLoc, keywordEnd);
                rewriter.ReplaceText(keywordRange, macroStart);
                
                // Add closing parenthesis after the operand
                if (coroStmt.operand) {
                    SourceLocation operandEnd = Lexer::getLocForEndOfToken(coroStmt.operandEnd, 0, sourceManager, LangOptions());
                    rewriter.InsertTextBefore(operandEnd, ")");
                }
                
            } else if (coroStmt.type == CoroutineStatement::AWAIT) {
                std::cout << "    DEBUG: Replacing co_await with CO_AWAIT(" << coroStmt.index << ", ...)\n";
                
                // Replace "co_await" with "CO_AWAIT(index, "
                std::string macroStart = "CO_AWAIT(" + std::to_string(coroStmt.index) + ", ";
                
                // Find the end of "co_await" keyword  
                SourceLocation keywordEnd = Lexer::getLocForEndOfToken(coroStmt.keywordLoc, 0, sourceManager, LangOptions());
                SourceRange keywordRange(coroStmt.keywordLoc, keywordEnd);
                rewriter.ReplaceText(keywordRange, macroStart);
                
                // Add closing parenthesis after the operand
                if (coroStmt.operand) {
                    SourceLocation operandEnd = Lexer::getLocForEndOfToken(coroStmt.operandEnd, 0, sourceManager, LangOptions());
                    rewriter.InsertTextBefore(operandEnd, ")");
                }
            }
        }
        
        std::cout << "  Applied " << coroutineStatements.size() << " coroutine statement replacements\n";
    }

public:
    void applyReplacements() {
        // First apply all the text replacements (declarations and references)
        std::vector<std::pair<SourceRange, std::string>> allReplacements;
        
        // Combine all replacements
        allReplacements.insert(allReplacements.end(), declReplacements.begin(), declReplacements.end());
        allReplacements.insert(allReplacements.end(), refReplacements.begin(), refReplacements.end());
        
        // Sort by source location in reverse order to avoid invalidating positions
        std::sort(allReplacements.begin(), allReplacements.end(), 
                 [&](const std::pair<SourceRange, std::string>& a, const std::pair<SourceRange, std::string>& b) {
                     return sourceManager.getFileOffset(a.first.getBegin()) > sourceManager.getFileOffset(b.first.getBegin());
                 });

        for (const auto& replacement : allReplacements) {
            std::cout << "  Applying replacement: " << replacement.second << "\n";
            rewriter.ReplaceText(replacement.first, replacement.second);
        }
        
        // Then apply destructor insertions
        std::sort(destructorInsertions.begin(), destructorInsertions.end(),
                 [&](const std::pair<SourceLocation, std::string>& a, const std::pair<SourceLocation, std::string>& b) {
                     return sourceManager.getFileOffset(a.first) > sourceManager.getFileOffset(b.first);
                 });
        
        for (const auto& insertion : destructorInsertions) {
            std::cout << "  Inserting destructors: " << insertion.second;
            rewriter.InsertTextBefore(insertion.first, insertion.second);
        }
        
        // Finally apply coroutine statement replacements
        applyCoroutineStatementReplacements();
        
        std::cout << "  Applied " << allReplacements.size() << " replacements, " 
                  << destructorInsertions.size() << " destructor insertions, and "
                  << coroutineStatements.size() << " coroutine statement replacements\n";
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
        
        if (coro.localVariables.empty()) {
            structCode += "    // No local variables found in this coroutine\n";
        } else {
            for (const auto& var : coro.localVariables) {
                structCode += "    _coro_storage<" + var.type + "> " + var.name + ";\n";
            }
        }
        
        structCode += "  };\n\n";
        
        // Generate the state machine class with run method header
        // The coroutine body will follow immediately after this
        structCode += "  class _detail_coro_statemachine_impl : public CoroImpl<_detail_coro_impl> {\n";
        structCode += "  public:\n";
        structCode += "    using CoroImpl<_detail_coro_impl>::CoroImpl; // Inherit constructors\n";
        structCode += "\n";
        structCode += "    void run() { ";
        
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

    void performRewrites() {
        for (const auto& coro : coroutines) {
            if (coro.hasError) {
                continue;
            }
            
            std::string structCode = generateCoroImplStruct(coro);
            
            if (coro.insertionPoint.isValid()) {
                rewriter.InsertTextBefore(coro.insertionPoint, structCode);
                std::cout << "Inserted _detail_coro_impl struct into " 
                         << coro.function->getQualifiedNameAsString() << "\n";
                
                // Now rewrite the coroutine body to use the storage wrappers
                rewriteCoroutineBody(coro);
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
            CoroutineBodyRewriter bodyRewriter(coro.localVariables, rewriter, sourceManager);
            
            // First traverse to collect all replacements
            bodyRewriter.TraverseStmt(const_cast<Stmt*>(bodyStmt));
            
            // Apply all the variable rewrites in place (construct/destroy calls, get() access)
            std::cout << "  DEBUG: Applying all variable transformations in place\n";
            bodyRewriter.applyReplacements();
            
            // Now wrap the transformed code with run() method braces
            std::cout << "  DEBUG: Wrapping transformed code with run() method\n";
            wrapBodyWithRunMethod(coro);
            
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
    

    void wrapBodyWithRunMethod(const CoroutineInfo& coro) {
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
                // Insert "} };" after the closing brace to close run method and class
                // Find the location right after the closing brace
                SourceLocation afterRbrace = Lexer::getLocForEndOfToken(rbraceLoc, 0, sourceManager, langOptions);
                if (afterRbrace.isValid()) {
                    std::string runMethodEnd = "\n  };\n }\n";
                    rewriter.InsertTextBefore(afterRbrace, runMethodEnd);
                    std::cout << "  DEBUG: Inserted closing braces after coroutine body\n";
                } else {
                    std::cout << "  ERROR: Could not find location after closing brace\n";
                }
                
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