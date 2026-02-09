//
// TryCatchStructures - Implementation
//

#include "TryCatchStructures.h"
#include "../Common.h"
#include <limits>

using namespace clang;

std::vector<std::tuple<SourceRange, std::string, int, bool>>
TryCatchBlock::generateReplacements(
    const SourceManager &sourceManager,
    std::function<SourceLocation(SourceLocation)> getLocForEndOfToken
) const {
    std::vector<std::tuple<SourceRange, std::string, int, bool>> replacements;

    REWRITE_LOG() << "\n=== COLLECTING TRY-CATCH REPLACEMENTS ===\n";
    REWRITE_LOG() << "    DEBUG: Processing try-catch block " << index << "\n";

    // Calculate inverted index once
    size_t inverted_index = std::numeric_limits<size_t>::max() - index;

    // Part 1: Replace "try" keyword with "TRY_BEGIN(index)"
    if (tryKeywordLoc.isValid()) {
        SourceLocation tryEnd = getLocForEndOfToken(tryKeywordLoc);
        SourceRange tryKeywordRange(tryKeywordLoc, tryEnd);

        std::string tryBegin = "TRY_BEGIN(" + std::to_string(inverted_index) + "ULL);";
        int tryPriority = 20000 + static_cast<int>(index);
        replacements.emplace_back(tryKeywordRange, tryBegin, tryPriority, true);

        REWRITE_LOG() << "        Added try keyword replacement with TRY_BEGIN(" << index << ") at "
                << tryKeywordRange.printToString(sourceManager) << " (priority " << tryPriority << ")\n";
    }

    // Part 2: Insert "TRY_END(index)" after the closing brace of try block
    if (tryBlockEnd.isValid()) {
        SourceLocation afterBrace = getLocForEndOfToken(tryBlockEnd);
        SourceRange endRange(afterBrace, afterBrace);

        std::string tryEnd = " TRY_END(" + std::to_string(inverted_index) + "ULL);";
        int endPriority = 30000 + static_cast<int>(index) + 1;
        replacements.emplace_back(endRange, tryEnd, endPriority, false);

        REWRITE_LOG() << "        Added TRY_END(" << index << ") insertion at "
                << endRange.printToString(sourceManager) << " (priority " << endPriority << ")\n";
    }

    // Part 3: Delete all catch clauses (from after try block to end of catch clauses)
    if (tryBlockEnd.isValid() && catchEnd.isValid()) {
        SourceLocation catchStart = getLocForEndOfToken(tryBlockEnd);
        SourceLocation catchEndAfter = getLocForEndOfToken(catchEnd);

        SourceRange catchRange(catchStart, catchEndAfter);

        // Replace all catch clauses with empty string
        int deletePriority = 20000 + static_cast<int>(index) + 2;
        replacements.emplace_back(catchRange, "", deletePriority, true);

        REWRITE_LOG() << "        Added catch clause deletion from "
                << catchRange.printToString(sourceManager) << " (priority " << deletePriority << ")\n";
    }

    REWRITE_LOG() << "=== END COLLECTING TRY-CATCH REPLACEMENTS ===\n\n";

    return replacements;
}
