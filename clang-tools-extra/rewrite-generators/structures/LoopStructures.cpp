//
// Implementation of loop rewriting structures
//

#include "LoopStructures.h"
#include "../Common.h"
#include "ScopeStructures.h"
#include "../codegen/MacroCodeGenerator.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"

using namespace clang;

RangedForReplacements RangedForLoop::collectReplacements(
    const SourceManager &SM,
    std::function<std::string(const Expr*)> rewriteExprFunc,
    std::function<SourceLocation(SourceLocation)> getLocForEndOfTokenFunc
) const {
    RangedForReplacements result;

    REWRITE_LOG() << "\n=== COLLECTING RANGED-FOR REPLACEMENTS ===\n";
    REWRITE_LOG() << "    DEBUG: Processing ranged-for loop " << index << "\n";

    // Collect all replacement parts
    collectHeaderReplacement(SM, rewriteExprFunc, result);
    collectLoopVarConstruct(SM, getLocForEndOfTokenFunc, result);
    collectLoopVarDestroy(SM, result);
    collectFooterInsertions(SM, result);

    REWRITE_LOG() << "=== END COLLECTING RANGED-FOR REPLACEMENTS ===\n\n";

    return result;
}

void RangedForLoop::collectHeaderReplacement(
    const SourceManager &SM,
    std::function<std::string(const Expr*)> rewriteExprFunc,
    RangedForReplacements &result
) const {
    REWRITE_LOG() << "      DEBUG: Collecting header replacement for ranged-for loop " << index << "\n";

    // Find the range from "for" keyword to the closing parenthesis
    const CXXForRangeStmt *forStmt = stmt;
    SourceLocation forLoc = forStmt->getForLoc();
    SourceLocation rParenLoc = forStmt->getRParenLoc();

    if (forLoc.isValid() && rParenLoc.isValid()) {
        SourceRange headerRange(forLoc, rParenLoc);

        // Generate the construct calls for __range, __begin, __end followed by the for loop
        // Use rewriteExpression to handle variable references in the range expression
        std::string rangeExprRewritten = rewriteExprFunc(rangeExpr);
        std::string constructCalls =
                makeStateConstructCall(rangeVarName, rangeExprRewritten) + ";\n" +
                "    " + makeStateConstructCall(beginVarName, makeStateGetCall(
                                                                            rangeVarName) + ".begin()") +
                ";\n" +
                "    " + makeStateConstructCall(endVarName, makeStateGetCall(
                                                                          rangeVarName) + ".end()") +
                ";\n" +
                "    for (; " + makeStateGetCall(beginVarName) + " != " + makeStateGetCall(
                    endVarName) +
                "; ++" + makeStateGetCall(beginVarName) + ")";

        int headerPriority = 10000 + static_cast<int>(index);
        result.globalReplacements.emplace_back(headerRange, constructCalls, headerPriority, true);

        REWRITE_LOG() << "        Added header replacement with construct calls at "
                << headerRange.printToString(SM) << " (priority " << headerPriority << ")\n";
        REWRITE_LOG() << "        Construct calls: " << constructCalls.substr(0, 100) << "...\n";
    } else {
        REWRITE_LOG() << "        ERROR: Invalid for statement locations\n";
    }
}

void RangedForLoop::collectLoopVarConstruct(
    const SourceManager &SM,
    std::function<SourceLocation(SourceLocation)> getLocForEndOfTokenFunc,
    RangedForReplacements &result
) const {
    REWRITE_LOG() << "      DEBUG: Collecting loop var construct for ranged-for loop " << index << "\n";

    // Find the opening brace of the loop body
    const Stmt *body = stmt->getBody();
    if (auto *compoundBody = dyn_cast<CompoundStmt>(body)) {
        SourceLocation lBraceLoc = compoundBody->getLBracLoc();

        if (lBraceLoc.isValid()) {
            // Insert after the opening brace
            SourceLocation insertLoc = getLocForEndOfTokenFunc(lBraceLoc);
            SourceRange insertRange(insertLoc, insertLoc);

            std::string constructCall = "\n    this->state." + loopVarName +
                                        ".construct(*" + makeStateGetCall(beginVarName) + ");";

            int constructPriority = 20000 + static_cast<int>(index);
            result.globalReplacements.emplace_back(insertRange, constructCall, constructPriority, true);

            REWRITE_LOG() << "        Added construct insertion: '" << constructCall << "' after "
                    << lBraceLoc.printToString(SM) << " (priority " << constructPriority << ")\n";
        } else {
            REWRITE_LOG() << "        ERROR: Invalid opening brace location\n";
        }
    } else {
        REWRITE_LOG() << "        ERROR: Loop body is not a compound statement\n";
    }
}

void RangedForLoop::collectLoopVarDestroy(
    const SourceManager &SM,
    RangedForReplacements &result
) const {
    REWRITE_LOG() << "      DEBUG: Collecting loop var destroy for ranged-for loop " << index << "\n";

    // Find the closing brace of the loop body
    const Stmt *body = stmt->getBody();
    if (auto *compoundBody = dyn_cast<CompoundStmt>(body)) {
        SourceLocation rBraceLoc = compoundBody->getRBracLoc();

        if (rBraceLoc.isValid()) {
            unsigned fileOffset = SM.getFileOffset(rBraceLoc);

            std::string destroyCall = "    " + makeStateDestroyCall(loopVarName) + ";\n";

            // Use high priority to ensure it comes after regular variable destructors
            int destroyPriority = 30000 + static_cast<int>(index);

            ScopeEndReplacement replacement;
            replacement.replacement = destroyCall;
            replacement.priority = destroyPriority;

            result.scopeEndReplacements[fileOffset].push_back(replacement);

            REWRITE_LOG() << "        Added destroy insertion: '" << destroyCall << "' before "
                    << rBraceLoc.printToString(SM) << " (priority " << destroyPriority
                    << ", file offset " << fileOffset << ")\n";
        } else {
            REWRITE_LOG() << "        ERROR: Invalid closing brace location\n";
        }
    } else {
        REWRITE_LOG() << "        ERROR: Loop body is not a compound statement\n";
    }
}

void RangedForLoop::collectFooterInsertions(
    const SourceManager &SM,
    RangedForReplacements &result
) const {
    REWRITE_LOG() << "      DEBUG: Collecting footer insertions for ranged-for loop " << index << "\n";

    // Find the location where footer destroy calls should be inserted
    // This should be AFTER the entire for loop statement (after its closing brace)
    const Stmt *body = stmt->getBody();
    if (body) {
        if (auto *compoundBody = dyn_cast<CompoundStmt>(body)) {
            // Calculate where the footer should go in the transformed code
            // In the explicit form, the footer goes after "  }" (the for loop close) but before "}" (scope close)
            SourceLocation footerLoc = compoundBody->getRBracLoc();
            unsigned fileOffset = SM.getFileOffset(footerLoc);

            // Create destroy calls for __end, __begin, __range (reverse order of construction)
            std::vector<std::string> destroyCalls = {
                "    " + makeStateDestroyCall(endVarName) + ";\n",
                "    " + makeStateDestroyCall(beginVarName) + ";\n",
                "    " + makeStateDestroyCall(rangeVarName) + ";\n"
            };

            // Add each destroy call with high priorities to ensure they come after loop variable destruction
            int basePriority = 50000 + static_cast<int>(index) * 10;

            for (size_t i = 0; i < destroyCalls.size(); ++i) {
                ScopeEndReplacement replacement;
                replacement.replacement = destroyCalls[i];
                replacement.priority = basePriority + static_cast<int>(i);
                replacement.insertAfterBrace = true;

                result.scopeEndReplacements[fileOffset].push_back(replacement);

                REWRITE_LOG() << "    Added footer destroy call: " << destroyCalls[i].substr(0, 40) << "... "
                        << "at " << footerLoc.printToString(SM)
                        << " (priority " << replacement.priority << ", file offset " << fileOffset << ")\n";
            }
        }
    }
}
