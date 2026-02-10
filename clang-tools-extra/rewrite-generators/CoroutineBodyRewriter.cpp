//
// CoroutineBodyRewriter - Implementation
//

#include "CoroutineBodyRewriter.h"
#include "LambdaRewriter.h"
#include "infrastructure/ASTHelpers.h"
#include "infrastructure/ReplacementApplicator.h"
#include "collectors/HelperCollectors.h"
#include "codegen/MacroCodeGenerator.h"
#include "clang/Lex/Lexer.h"

#include <sstream>
#include <algorithm>

static bool hasCoroutineKeywords(const Stmt *stmt) {
    if (!stmt) return false;
    if (isa<CoawaitExpr>(stmt) || isa<CoyieldExpr>(stmt) || isa<CoreturnStmt>(stmt))
        return true;
    for (auto it = stmt->child_begin(); it != stmt->child_end(); ++it)
        if (*it && hasCoroutineKeywords(*it)) return true;
    return false;
}

CoroutineBodyRewriter::CoroutineBodyRewriter(const std::set<LocalVariable> &vars, Rewriter &rewr, const SourceManager &SM,
                          CoroutineInfo &coroInfo, ASTContext &astCtx, bool isMember, const CXXRecordDecl *classRecord)
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
void CoroutineBodyRewriter::buildDeclLocationMapping() {
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

    // Helper function to get the location after the end of a token
    // Wraps Lexer::getLocForEndOfToken with consistent parameters
SourceLocation CoroutineBodyRewriter::getLocForEndOfToken(SourceLocation loc) {
        return Lexer::getLocForEndOfToken(loc, 0, sourceManager, LangOptions());
    }

    // Track compound statements (scopes) - using Traverse for proper pre/post hooks
bool CoroutineBodyRewriter::TraverseCompoundStmt(CompoundStmt *compoundStmt) {
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
bool CoroutineBodyRewriter::VisitDeclStmt(DeclStmt *declStmt) {
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
                                SourceLocation afterLastChild = getLocForEndOfToken(childrenRange->getEnd());
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
                                SourceLocation afterLastChild = getLocForEndOfToken(childrenRange->getEnd());

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
                            SourceLocation afterInit = getLocForEndOfToken(initEnd);
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
bool CoroutineBodyRewriter::VisitCoyieldExpr(CoyieldExpr *coyield) {
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

            // Collect subexpression temporaries (existing behavior)
            // TODO<joka921> here we try the simplification for nested temporaries.
            //collectTemporariesFromExpression(coroStmt.operand, coroStmt);
            collectTemporariesFromExpression(coyield->getOperand(), coroStmt);

            // Check if the operand itself is a temporary (prvalue)
            /*
            if (isPrValue(coroStmt.operand)) {
                // Create a TemporaryInfo for the top-level temporary
                TemporaryInfo topLevelTemp;
                topLevelTemp.expr = nullptr;  // No explicit MaterializeTemporaryExpr
                topLevelTemp.tempVarName = "temp_" + std::to_string(coroStmt.index) + "_toplevel";
                topLevelTemp.type = coroStmt.operand->getType();
                topLevelTemp.constructLoc = coroStmt.operand->getBeginLoc();
                topLevelTemp.isBracedInit = false;  // Always use paren init
                topLevelTemp.initArgs = "";  // Handled via range replacement

                REWRITE_LOG() << "    DEBUG: co_yield operand is a prvalue, creating top-level temporary "
                             << topLevelTemp.tempVarName << " of type "
                             << typeAsString(topLevelTemp.type, *astContext) << "\n";

                coroStmt.temporaries.push_back(topLevelTemp);
            }
            */

            // Add temporary variable names to alive variables
            for (const auto &temp : coroStmt.temporaries) {
                coroStmt.aliveVariables.push_back(temp.tempVarName);
                REWRITE_LOG() << "      DEBUG: Temporary '" << temp.tempVarName
                             << "' is alive at suspension point " << coroStmt.index << "\n";
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

        coroutineStatements.push_back(coroStmt);

        REWRITE_LOG() << "    DEBUG: Added co_yield with index " << coroStmt.index << " with " << coroStmt.aliveVariables.size() << " alive variables\n";

        return true;
    }

    // Handle co_await expressions
bool CoroutineBodyRewriter::VisitCoawaitExpr(CoawaitExpr *coawait) {
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

            // Collect subexpression temporaries (existing behavior)
            collectTemporariesFromExpression(coroStmt.operand, coroStmt);

            // Check if the operand itself is a temporary (prvalue)
            if (isPrValue(coroStmt.operand)) {
                // Create a TemporaryInfo for the top-level temporary
                TemporaryInfo topLevelTemp;
                topLevelTemp.expr = nullptr;  // No explicit MaterializeTemporaryExpr
                topLevelTemp.tempVarName = "temp_" + std::to_string(coroStmt.index) + "_toplevel";
                topLevelTemp.type = coroStmt.operand->getType();
                topLevelTemp.constructLoc = coroStmt.operand->getBeginLoc();
                topLevelTemp.isBracedInit = false;  // Always use paren init
                topLevelTemp.initArgs = "";  // Handled via range replacement

                REWRITE_LOG() << "    DEBUG: co_await operand is a prvalue, creating top-level temporary "
                             << topLevelTemp.tempVarName << " of type "
                             << typeAsString(topLevelTemp.type, *astContext) << "\n";

                coroStmt.temporaries.push_back(topLevelTemp);
            }

            // Add temporary variable names to alive variables
            for (const auto &temp : coroStmt.temporaries) {
                coroStmt.aliveVariables.push_back(temp.tempVarName);
                REWRITE_LOG() << "      DEBUG: Temporary '" << temp.tempVarName
                             << "' is alive at suspension point " << coroStmt.index << "\n";
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

        coroutineStatements.push_back(coroStmt);

        REWRITE_LOG() << "    DEBUG: Added co_await with index " << coroStmt.index << " with " << coroStmt.aliveVariables.size() << " alive variables\n";

        return true;
    }

    // Handle co_return statements
bool CoroutineBodyRewriter::VisitCoreturnStmt(CoreturnStmt *coreturn) {
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
bool CoroutineBodyRewriter::TraverseCXXTryStmt(CXXTryStmt *tryStmt) {
        // ===== PRE-TRAVERSAL =====
        REWRITE_LOG() << "  Found try-catch block\n";

        TryCatchBlock tryCatch;
        tryCatch.tryStmt = tryStmt;
        tryCatch.index = nextTryCatchIndex++;
        tryCatch.resumeIndex = nextCoroStatementIndex++;

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
std::string CoroutineBodyRewriter::processCatchClause(const CXXCatchStmt *catchStmt) {
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
            std::string replacement = makeGetCallInsideState(memberName);

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
std::string CoroutineBodyRewriter::transformVariableReferencesInText(const std::string &text) {
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
bool CoroutineBodyRewriter::TraverseCXXForRangeStmt(CXXForRangeStmt *forRange) {
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
bool CoroutineBodyRewriter::VisitDeclRefExpr(DeclRefExpr *declRef) {
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
bool CoroutineBodyRewriter::VisitMemberExpr(MemberExpr *memberExpr) {
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
bool CoroutineBodyRewriter::VisitCXXThisExpr(CXXThisExpr *thisExpr) {
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
bool CoroutineBodyRewriter::TraverseForStmt(ForStmt *forStmt) {
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
bool CoroutineBodyRewriter::TraverseWhileStmt(WhileStmt *whileStmt) {
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
bool CoroutineBodyRewriter::TraverseDoStmt(DoStmt *doStmt) {
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
bool CoroutineBodyRewriter::VisitBreakStmt(BreakStmt *breakStmt) {
        REWRITE_LOG() << "  Found break statement\n";

        // Find the innermost loop and destroy variables in its scope
        insertLoopVariableDestructors(breakStmt, true /* is break */);

        return true;
    }

    // Handle continue statements - need to destroy variables in loop scope
bool CoroutineBodyRewriter::VisitContinueStmt(ContinueStmt *continueStmt) {
        REWRITE_LOG() << "  Found continue statement\n";

        // Find the innermost loop and destroy variables in its scope
        insertLoopVariableDestructors(continueStmt, false /* is continue */);

        return true;
    }


bool CoroutineBodyRewriter::TraverseLambdaExpr(LambdaExpr *lambdaExpr) {
        // Skip lambda coroutines - they are handled by CoroutineRewriter::VisitLambdaExpr
        const CXXMethodDecl *callOperator = lambdaExpr->getCallOperator();
        if (callOperator && callOperator->hasBody() &&
            hasCoroutineKeywords(callOperator->getBody())) {
            REWRITE_LOG() << "  DEBUG: Skipping coroutine lambda (handled by CoroutineRewriter)\n";
            return true;
        }

        REWRITE_LOG() << "  DEBUG: Processing regular lambda in coroutine body\n";

        // Use LambdaRewriter to get the struct definition and class name
        LambdaRewriter lambdaRewriter(sourceManager, *astContext);
        LambdaRewriteResult rewriteResult = lambdaRewriter.rewriteLambda(lambdaExpr);

        // Now build a constructor call with CO_GET-transformed capture init expressions
        std::vector<CaptureInfo> captures = lambdaRewriter.analyzeCaptures(lambdaExpr);

        std::string constructorCall = rewriteResult.className + "{";
        auto initIt = lambdaExpr->capture_init_begin();
        bool firstArg = true;
        for (size_t i = 0; i < captures.size(); ++i) {
            const CaptureInfo &cap = captures[i];
            const Expr *initExpr = *initIt;
            ++initIt;

            if (!firstArg) constructorCall += ", ";
            firstArg = false;

            if (cap.isThisCapture) {
                constructorCall += isMemberFunction ? "__self" : "this";
            } else if (cap.isStarThisCapture) {
                constructorCall += isMemberFunction ? "*__self" : "*this";
            } else if (initExpr) {
                // Apply CO_GET transformation to the init expression
                constructorCall += rewriteExpression(initExpr);
            } else {
                constructorCall += cap.initExpression;
            }
        }
        constructorCall += "}";

        REWRITE_LOG() << "  DEBUG: Lambda class name: " << rewriteResult.className << "\n";
        REWRITE_LOG() << "  DEBUG: Lambda constructor call: " << constructorCall << "\n";

        // Add a global replacement for the lambda expression source range
        SourceRange lambdaRange = lambdaExpr->getSourceRange();
        int fileOffset = sourceManager.getFileOffset(lambdaRange.getBegin());
        globalReplacements.emplace_back(lambdaRange, constructorCall, fileOffset, true);

        // Store the lambda info for later insertion before the function
        LambdaInCoroutine lambdaInfo;
        lambdaInfo.classDefinition = rewriteResult.classDefinition;
        lambdaInfo.constructorCall = constructorCall;
        lambdaInfo.className = rewriteResult.className;
        lambdaInfo.lambdaSourceRange = lambdaRange;
        collectedLambdas.push_back(lambdaInfo);

        // Return true WITHOUT calling base TraverseLambdaExpr - skip recursion into lambda body
        return true;
    }

    enum InitializationForm {
        CONSTRUCT_CALL, // Regular construct() call for other types
        BRACED_INIT,    // Braced initialization like {1, 2}
        PAREN_INIT      // Parenthesized initialization like (1, 2)
    };

CoroutineBodyRewriter::InitializationForm CoroutineBodyRewriter::getInitializationForm(const VarDecl *varDecl) {
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

std::optional<SourceRange> CoroutineBodyRewriter::handleParenthesizedInitialization(const VarDecl *varDecl) {
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
SourceLocation CoroutineBodyRewriter::findClosingParen(SourceLocation startLoc) {
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

void CoroutineBodyRewriter::processParenthesizedArguments(const VarDecl *varDecl) {
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

void CoroutineBodyRewriter::insertLoopVariableDestructors(const Stmt *stmt, bool isBreak) {
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

std::optional<SourceRange> CoroutineBodyRewriter::handleBracedInitialization(const VarDecl *varDecl) {
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

std::string CoroutineBodyRewriter::getInitializationArguments(const VarDecl *varDecl) {
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

std::string CoroutineBodyRewriter::processInitExpression(const Expr *init, const std::string &currentVarName) {
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
void CoroutineBodyRewriter::collectTemporariesFromExpression(const Expr *expr, CoroutineStatement &coroStmt) {
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

std::string CoroutineBodyRewriter::rewriteVariableReferencesInText(const std::string &text, const std::string &excludeVar) {
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

std::string CoroutineBodyRewriter::handleConstructorArgs(const CXXConstructExpr *constructExpr) {
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

std::string CoroutineBodyRewriter::handleInitListArgs(const InitListExpr *initList) {
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

std::string CoroutineBodyRewriter::rewriteExpression(const Expr *expr) {
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

std::string CoroutineBodyRewriter::rewriteExpressionExceptVar(const Expr *expr, const std::string &excludeVar) {
        if (!expr) return "";

        SourceRange range = expr->getSourceRange();
        CharSourceRange charRange = CharSourceRange::getTokenRange(range);
        std::string exprText = Lexer::getSourceText(charRange, sourceManager, LangOptions()).str();

        REWRITE_LOG() << "        DEBUG: rewriteExpressionExceptVar input: '" << exprText << "'\n";
        REWRITE_LOG() << "        DEBUG: Expression type: " << expr->getStmtClassName() << "\n";
        REWRITE_LOG() << "        DEBUG: Excluding variable: '" << excludeVar << "'\n";

        return rewriteVariableReferencesInText(exprText, excludeVar);
    }

void CoroutineBodyRewriter::insertDestructorsForScope(const ScopeInfo &scope) {
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

bool CoroutineBodyRewriter::isPartOfDeclaration(const DeclRefExpr *declRef) {
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

void CoroutineBodyRewriter::collectCoroutineStatementReplacements() {
        REWRITE_LOG() << "  DEBUG: Collecting coroutine statement replacements for " << coroutineStatements.size() <<
                " statements\n";

        for (const auto &coroStmt: coroutineStatements) {
            auto replacements = coroStmt.generateReplacements(sourceManager, *astContext);
            globalReplacements.insert(globalReplacements.end(), replacements.begin(), replacements.end());
        }

        REWRITE_LOG() << "  Collected " << coroutineStatements.size() <<
                " coroutine statement replacements into global vector\n";
    }


void CoroutineBodyRewriter::collectRangedForLoopReplacements() {
        REWRITE_LOG() << "  DEBUG: Collecting ranged-for loop replacements for " << rangedForLoops.size() << " loops\n";

        for (const auto &rangedFor: rangedForLoops) {
            // Delegate all replacement collection to the RangedForLoop struct
            auto replacements = rangedFor.collectReplacements(
                sourceManager,
                [this](const Expr* expr) { return rewriteExpression(expr); },
                [this](SourceLocation loc) { return getLocForEndOfToken(loc); }
            );

            // Merge global replacements
            globalReplacements.insert(
                globalReplacements.end(),
                replacements.globalReplacements.begin(),
                replacements.globalReplacements.end()
            );

            // Merge scope end replacements
            for (const auto &[fileOffset, replacementList] : replacements.scopeEndReplacements) {
                scopeEndReplacements[fileOffset].insert(
                    scopeEndReplacements[fileOffset].end(),
                    replacementList.begin(),
                    replacementList.end()
                );
            }
        }

        REWRITE_LOG() << "  Collected " << rangedForLoops.size() << " ranged-for loop replacements\n";
    }

std::string CoroutineBodyRewriter::generateExplicitLoopForm(const RangedForLoop &rangedFor) {
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


void CoroutineBodyRewriter::collectScopeEndReplacements() {
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


void CoroutineBodyRewriter::collectTryCatchReplacements() {
        REWRITE_LOG() << "  DEBUG: Collecting try-catch replacements for " << tryCatchBlocks.size() << " blocks\n";

        for (const auto &tryCatch: tryCatchBlocks) {
            // Delegate to the TryCatchBlock member function
            auto replacements = tryCatch.generateReplacements(
                sourceManager,
                [this](SourceLocation loc) { return this->getLocForEndOfToken(loc); }
            );

            // Add all replacements to globalReplacements
            globalReplacements.insert(globalReplacements.end(),
                                      replacements.begin(),
                                      replacements.end());
        }

        REWRITE_LOG() << "  Collected " << tryCatchBlocks.size() << " try-catch replacements\n";
    }

void CoroutineBodyRewriter::applyReplacements() {
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

void CoroutineBodyRewriter::collectRangeReplacements() {
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
    }

void CoroutineBodyRewriter::applyAllReplacements() {
        // Use the generic ReplacementApplicator infrastructure
        ReplacementApplicator applicator(rewriter, sourceManager);
        applicator.applyReplacements(globalReplacements);
    }

