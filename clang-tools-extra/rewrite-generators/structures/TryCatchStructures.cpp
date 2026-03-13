//
// TryCatchStructures - Implementation
//

#include "TryCatchStructures.h"
#include "../Common.h"

using namespace clang;

void TryCatchBlock::collectTransitiveStates(unsigned blockIndex,
                                            const std::vector<TryCatchBlock> &allBlocks,
                                            std::vector<unsigned> &out) {
    const TryCatchBlock &block = allBlocks[blockIndex];
    // Add direct suspension indices
    for (unsigned idx : block.directSuspensionIndices) {
        out.push_back(idx);
    }
    // Recurse into nested children
    for (unsigned childIdx : block.nestedTryIndices) {
        collectTransitiveStates(childIdx, allBlocks, out);
    }
}

std::vector<std::tuple<SourceRange, std::string, int, bool>>
TryCatchBlock::generateReplacements(
    const SourceManager &sourceManager,
    std::function<SourceLocation(SourceLocation)> getLocForEndOfToken,
    const std::vector<TryCatchBlock> &allBlocks
) const {
    std::vector<std::tuple<SourceRange, std::string, int, bool>> replacements;

    REWRITE_LOG() << "\n=== COLLECTING TRY-CATCH REPLACEMENTS ===\n";
    REWRITE_LOG() << "    DEBUG: Processing try-catch block " << index << "\n";

    // Part 1: Insert "resume_try_N:" label before the "try" keyword
    if (tryKeywordLoc.isValid()) {
        SourceRange beforeTry(tryKeywordLoc, tryKeywordLoc);
        std::string label = "resume_try_" + std::to_string(index) + ": ";
        int priority = 20000 + static_cast<int>(index);
        replacements.emplace_back(beforeTry, label + "try", priority, true);

        REWRITE_LOG() << "        Added resume_try_" << index << " label at "
                << beforeTry.printToString(sourceManager) << " (priority " << priority << ")\n";
    }

    // Part 2: After opening '{' of try body, insert inner try { switch(this->curState) { cases... default: break; }
    if (tryBlockStart.isValid()) {
        SourceLocation afterBrace = getLocForEndOfToken(tryBlockStart);
        SourceRange insertRange(afterBrace, afterBrace);

        std::string innerSwitch = "\ntry {\nswitch(this->curState) {\n";

        // Add cases for direct suspension indices
        for (unsigned sIdx : directSuspensionIndices) {
            innerSwitch += "  case " + std::to_string(sIdx) + ": goto label_" + std::to_string(sIdx) + ";\n";
        }

        // Add cases for states transitively inside nested try blocks — route to their resume_try label
        for (unsigned childIdx : nestedTryIndices) {
            std::vector<unsigned> nestedStates;
            collectTransitiveStates(childIdx, allBlocks, nestedStates);
            for (unsigned sIdx : nestedStates) {
                innerSwitch += "  case " + std::to_string(sIdx) + ": goto resume_try_" + std::to_string(childIdx) + ";\n";
            }
        }

        innerSwitch += "  default: break;\n}\n";

        int priority = 20000 + static_cast<int>(index) + 1;
        replacements.emplace_back(insertRange, innerSwitch, priority, false);

        REWRITE_LOG() << "        Added inner switch dispatch at "
                << insertRange.printToString(sourceManager) << " (priority " << priority << ")\n";
    }

    // Part 3: At closing '}' of try body, insert the inner catch clause.
    // The scope-end system replaces the original '}' with "destructors; }".
    // That '}' closes the inner try (opened in Part 2). We append:
    //   " catch (...) { cleanup; throw; }" (inner catch, attaches to inner try's })
    //   "}" (closes the outer try body, replacing the original brace)
    if (tryBlockEnd.isValid()) {
        SourceRange atClose(tryBlockEnd, tryBlockEnd);

        std::string cleanup = " catch (...) {\n";
        // Destroy variables in reverse declaration order
        for (auto it = variablesInTryBlock.rbegin(); it != variablesInTryBlock.rend(); ++it) {
            cleanup += "  DESTROY_IF_CONSTRUCTED(" + *it + ");\n";
        }
        cleanup += "  throw;\n}\n}\n";

        int priority = 20000 + static_cast<int>(index) + 2;
        replacements.emplace_back(atClose, cleanup, priority, false);

        REWRITE_LOG() << "        Added inner catch cleanup at "
                << atClose.printToString(sourceManager) << " (priority " << priority << ")\n";
    }

    REWRITE_LOG() << "=== END COLLECTING TRY-CATCH REPLACEMENTS ===\n\n";

    return replacements;
}
