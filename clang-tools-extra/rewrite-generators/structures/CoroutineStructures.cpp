//
// CoroutineStructures - Implementation
//

#include "CoroutineStructures.h"
#include "../Common.h"
#include "../codegen/MacroCodeGenerator.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Tooling.h"
#include "../Helpers.h"

#include <optional>

using namespace clang;

std::vector<std::tuple<SourceRange, std::string, int, bool>>
CoroutineStatement::generateReplacements(
    const SourceManager &sourceManager,
    ASTContext &astContext
) const {
    std::vector<std::tuple<SourceRange, std::string, int, bool>> replacements;

    switch (type) {
        case YIELD:
            return generateYieldOrAwaitReplacements(sourceManager, astContext, "CO_YIELD");
        case AWAIT:
            return generateYieldOrAwaitReplacements(sourceManager, astContext, "CO_AWAIT");
        case RETURN:
            return generateReturnReplacements(sourceManager, astContext);
    }

    return replacements;
}

std::vector<std::tuple<SourceRange, std::string, int, bool>>
CoroutineStatement::generateYieldOrAwaitReplacements(
    const SourceManager &sourceManager,
    ASTContext &astContext,
    const std::string &macroBaseName
) const {
    std::vector<std::tuple<SourceRange, std::string, int, bool>> replacements;

    // Priority based on index for consistent ordering
    int priority = static_cast<int>(index);

    // Identify top-level temporary (if any)
    const TemporaryInfo *topLevelTemp = nullptr;
    std::vector<const TemporaryInfo*> subExprTemps;

    for (const auto &temp : temporaries) {
        if (temp.tempVarName.find("_toplevel") != std::string::npos) {
            topLevelTemp = &temp;
        } else {
            subExprTemps.push_back(&temp);
        }
    }

    // Determine which macro to use
    std::string macroName;
    std::string macroStart;

    if (topLevelTemp) {
        // Use CO_YIELD_TOPLEVEL(tempname, index, awaiterMem, init_expr)
        macroName = macroBaseName + "_TOPLEVEL";
        macroStart = macroName + "(" + topLevelTemp->tempVarName + ", " + std::to_string(index) + ", " + awaiterMemberName + ", ";
        REWRITE_LOG() << "    DEBUG: Using " << macroName << " for top-level temporary " << topLevelTemp->tempVarName << "\n";
    } else {
        // Use regular CO_YIELD(index, awaiterMem, operand) or CO_AWAIT_VOID(index, awaiterMem, operand)
        macroName = (macroBaseName == "CO_AWAIT") ? "CO_AWAIT_VOID" : macroBaseName;
        macroStart = macroName + "(" + std::to_string(index) + ", " + awaiterMemberName + ", ";
        REWRITE_LOG() << "    DEBUG: Using " << macroName << " (no top-level temporary)\n";
    }

    // Replace just the keyword
    SourceLocation keywordEnd = Lexer::getLocForEndOfToken(keywordLoc, 0, sourceManager, LangOptions());
    SourceRange keywordRange(keywordLoc, keywordEnd);
    replacements.emplace_back(keywordRange, macroStart, priority, true);

    // Process subexpression temporaries - use incremental replacements for recursive processing
    // Process in reverse order (innermost first) by assigning lower priorities to later temps
    for (size_t tempIdxB = 0; tempIdxB < subExprTemps.size(); ++tempIdxB) {
        size_t tempIdx = subExprTemps.size() - tempIdxB - 1;
        const auto *temp = subExprTemps[tempIdx];
        REWRITE_LOG() << "      DEBUG: Processing temporary '" << temp->tempVarName << "' (index " << tempIdx << ")\n";

        std::string initMacroName = temp->isBracedInit ? "CO_BRACED_INIT_OWNING" : "CO_PAREN_INIT_OWNING";
        std::string macroOpening = initMacroName + "(" + temp->tempVarName + ", ";

        // Each temporary gets its own priority range to avoid conflicts
        // Innermost temps (later in the vector) get lower priorities so they're processed first
        //int tempPriorityBase = priority + 100 + (int)(subExprTemps.size() - tempIdx - 1) * 10;
        int tempPriorityBase = priority + 100 + (int)(tempIdx) * 10;

        // Get the subexpression from the MaterializeTemporaryExpr
        const Expr *tempSubExpr = temp->expr->getSubExpr();

        if (temp->isBracedInit) {
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
                replacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                // Delete the opening brace
                SourceLocation lbrace = initList->getLBraceLoc();
                SourceLocation afterLBrace = lbrace.getLocWithOffset(1);
                SourceRange lbraceRange(lbrace, afterLBrace);
                replacements.emplace_back(lbraceRange, "", tempPriorityBase + 1, true);

                // Delete the closing brace and insert closing paren
                SourceLocation rbrace = initList->getRBraceLoc();
                SourceLocation afterRBrace = rbrace.getLocWithOffset(1);
                SourceRange rbraceRange(rbrace, afterRBrace);
                replacements.emplace_back(rbraceRange, ")", tempPriorityBase + 2, true);

                REWRITE_LOG() << "        DEBUG: Will use incremental replacements for braced init temp (priority " << tempPriorityBase << ")\n";
            } else {
                // Fallback: wrap the entire expression
                SourceLocation tempStart = tempSubExpr->getBeginLoc();
                SourceLocation tempEnd = Lexer::getLocForEndOfToken(tempSubExpr->getEndLoc(), 0, sourceManager, LangOptions());

                SourceRange macroStartRange(tempStart, tempStart);
                replacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                SourceRange macroEndRange(tempEnd, tempEnd);
                replacements.emplace_back(macroEndRange, ")", tempPriorityBase + 2, false);

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
                    replacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, true);

                    /*
                    // Delete the opening paren
                    SourceLocation afterLParen = lparenLoc.getLocWithOffset(1);
                    SourceRange lparenRange(lparenLoc, afterLParen);
                    replacements.emplace_back(lparenRange, "", tempPriorityBase + 1, true);
                    */

                    // Delete the closing paren and insert closing paren for macro
                    /*
                    SourceLocation afterRParen = Lexer::getLocForEndOfToken(rparenLoc, 0, sourceManager, LangOptions());
                    SourceRange rparenRange(rparenLoc, afterRParen);
                    replacements.emplace_back(rparenRange, ")", tempPriorityBase + 2, true);
                    */

                    REWRITE_LOG() << "        DEBUG: Will strip parens and use CO_PAREN_INIT_OWNING for ParenExpr (priority " << tempPriorityBase << ")\n";
                } else {
                    // Fallback: just wrap
                    SourceLocation tempStart = tempSubExpr->getBeginLoc();
                    SourceLocation tempEnd = Lexer::getLocForEndOfToken(tempSubExpr->getEndLoc(), 0, sourceManager, LangOptions());

                    SourceRange macroStartRange(tempStart, tempStart);
                    replacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                    SourceRange macroEndRange(tempEnd, tempEnd);
                    replacements.emplace_back(macroEndRange, ")", tempPriorityBase + 2, false);

                    REWRITE_LOG() << "        DEBUG: Will wrap ParenExpr (fallback, priority " << tempPriorityBase << ")\n";
                }
            } else {
                // Not a paren expression - just wrap
                SourceLocation tempStart = tempSubExpr->getBeginLoc();
                SourceLocation tempEnd = Lexer::getLocForEndOfToken(tempSubExpr->getEndLoc(), 0, sourceManager, LangOptions());

                SourceRange macroStartRange(tempStart, tempStart);
                replacements.emplace_back(macroStartRange, macroOpening, tempPriorityBase, false);

                SourceRange macroEndRange(tempEnd, tempEnd);
                replacements.emplace_back(macroEndRange, ")", tempPriorityBase + 2, false);

                REWRITE_LOG() << "        DEBUG: Will wrap general expression temp (priority " << tempPriorityBase << ")\n";
            }
        }
    }

    // Insert closing parenthesis after the operand (before the semicolon)
    if (operand) {
        SourceLocation operandEndLoc = Lexer::getLocForEndOfToken(operandEnd, 0, sourceManager, LangOptions());

        // Insert closing paren before the semicolon (right after the operand)
        SourceRange parenRange(operandEndLoc, operandEndLoc);
        replacements.emplace_back(parenRange, ")", priority + 1000, false);
    } else {
        // No operand case - just insert closing paren right after the macro start
        SourceRange parenRange(keywordEnd, keywordEnd);
        replacements.emplace_back(parenRange, ")", priority + 1000, false);
    }

    // Find the location to insert destructor calls (after the semicolon)
    // Strategy: start from statement end, skip any semicolons, and insert before the first non-semicolon token
    SourceLocation stmtEnd = stmt->getEndLoc();
    SourceLocation searchStart = Lexer::getLocForEndOfToken(stmtEnd, 0, sourceManager, LangOptions());

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
            searchStart = Lexer::getLocForEndOfToken(nextTokOpt->getLocation(), 0, sourceManager, LangOptions());
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
    if (!temporaries.empty()) {
        REWRITE_LOG() << "      DEBUG: Adding destruction calls for " << temporaries.size() << " temporaries\n";

        std::string destructorCalls = "\n";
        // Destroy in reverse order (last created first destroyed)
        for (auto it = temporaries.rbegin(); it != temporaries.rend(); ++it) {
            destructorCalls += "        this->" + it->tempVarName + ".destroy();\n";
        }

        // Insert after the semicolon
        SourceRange afterSemiRange(insertionPointAfterSemicolon, insertionPointAfterSemicolon);
        replacements.emplace_back(afterSemiRange, destructorCalls, priority + 2000, false);
    }

    for (const auto& [range, repl, priority, empty] : replacements) {
        REWRITE_LOG() << "Found coroStmtReplacement " <<  range.printToString(sourceManager) << " " << repl << " " << priority << " " << empty << "\n";
    }
    return replacements;

}

std::vector<std::tuple<SourceRange, std::string, int, bool>>
CoroutineStatement::generateReturnReplacements(
    const SourceManager &sourceManager,
    ASTContext &astContext
) const {
    std::vector<std::tuple<SourceRange, std::string, int, bool>> replacements;

    REWRITE_LOG() << "    DEBUG: Collecting co_return replacement for index " << index << "\n";

    int priority = static_cast<int>(index);
    SourceLocation keywordEnd = Lexer::getLocForEndOfToken(keywordLoc, 0, sourceManager, LangOptions());

    if (!operand) {
        // Case 1: co_return; (no operand) -> CO_RETURN_VOID(index);
        REWRITE_LOG() << "      DEBUG: co_return has no operand, using CO_RETURN_VOID\n";

        std::string replacement = "CO_RETURN_VOID(" + std::to_string(index) + ", __final_awaiter);";
        SourceRange keywordRange(keywordLoc, keywordEnd);
        replacements.emplace_back(keywordRange, replacement, priority, true);
    } else {
        // Check if operand type is void
        QualType operandType = operand->getType();
        bool isVoidType = operandType->isVoidType();

        if (isVoidType) {
            // Case 2: co_return expr; where expr is void -> expr; CO_RETURN_VOID(index, __final_awaiter);
            REWRITE_LOG() << "      DEBUG: co_return operand is void type, using CO_RETURN_VOID\n";

            // Replace "co_return " with nothing (just remove the keyword and space)
            SourceRange keywordRange(keywordLoc, keywordEnd);
            replacements.emplace_back(keywordRange, "", priority, true);

            // Insert "; CO_RETURN_VOID(index, __final_awaiter)" after the operand
            SourceLocation operandEndLoc = Lexer::getLocForEndOfToken(operandEnd, 0, sourceManager, LangOptions());
            SourceRange insertRange(operandEndLoc, operandEndLoc);
            std::string insertion = "; CO_RETURN_VOID(" + std::to_string(index) + ", __final_awaiter)";
            replacements.emplace_back(insertRange, insertion, priority + 1000, false);
        } else {
            // Case 3: co_return expr; where expr is non-void -> CO_RETURN_VALUE(index, __final_awaiter, (expr))
            REWRITE_LOG() << "      DEBUG: co_return operand is value type, using CO_RETURN_VALUE\n";

            // Replace "co_return" with "CO_RETURN_VALUE(index, __final_awaiter, ("
            std::string macroStart = "CO_RETURN_VALUE(" + std::to_string(index) + ", __final_awaiter, (";
            SourceRange keywordRange(keywordLoc, keywordEnd);
            replacements.emplace_back(keywordRange, macroStart, priority, true);

            // Insert "))" after the operand
            SourceLocation operandEndLoc = Lexer::getLocForEndOfToken(operandEnd, 0, sourceManager, LangOptions());
            SourceRange parenRange(operandEndLoc, operandEndLoc);
            replacements.emplace_back(parenRange, "))", priority + 1000, false);
        }
    }

    return replacements;
}
