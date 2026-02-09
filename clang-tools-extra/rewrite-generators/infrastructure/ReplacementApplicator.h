//
// Generic utility for applying prioritized source code replacements
// Reusable across any Clang/LLVM rewriting tool
//

#ifndef LLVM_REWRITE_GENERATORS_REPLACEMENTAPPLICATOR_H
#define LLVM_REWRITE_GENERATORS_REPLACEMENTAPPLICATOR_H

#include "clang/Basic/SourceManager.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "../Common.h"
#include <algorithm>
#include <optional>
#include <vector>

using namespace clang;

class ReplacementApplicator {
private:
    Rewriter &rewriter;
    const SourceManager &sourceManager;

public:
    ReplacementApplicator(Rewriter &rewr, const SourceManager &SM)
        : rewriter(rewr), sourceManager(SM) {}

    // Apply a vector of prioritized replacements
    // Replacements are sorted by position (reverse order) and priority (ascending)
    // Overlapping or adjacent replacements are combined
    void applyReplacements(std::vector<PrioritizedReplacement> &replacements) {
        REWRITE_LOG() << "\n=== APPLYING ALL REPLACEMENTS WITH PRIORITY ===\n";
        REWRITE_LOG() << "  DEBUG: Sorting and applying " << replacements.size() << " replacements\n";

        // Sort ALL replacements by position first, then by priority
        std::stable_sort(replacements.begin(), replacements.end(),
                         [&](const auto &a, const auto &b) {
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
                                 llvm::errs() << "Trying to insert at the same position, this shouldn't happen.\n";
                             }
                             return std::get<1>(a) < std::get<1>(b); // Sort by replacement text
                         });

        // Deduplicate redundant replacements
        replacements.erase(std::unique(replacements.begin(), replacements.end()),
                          replacements.end());

        auto doRewrite = [&](const auto &buf) {
            bool isEmpty = !std::get<3>(buf);
            REWRITE_LOG() << " Applying at " << std::get<0>(buf).printToString(sourceManager)
                    << " (priority " << std::get<2>(buf) << "), empty: " << isEmpty
                    << ": '" << std::get<1>(buf) << "'\n";
            if (isEmpty) {
                rewriter.InsertText(std::get<0>(buf).getBegin(), std::get<1>(buf));
            } else {
                rewriter.ReplaceText(std::get<0>(buf), std::get<1>(buf));
            }
        };

        // Apply all replacements, combining overlapping/adjacent ones
        std::optional<PrioritizedReplacement> buffer;
        for (size_t i = 0; i < replacements.size(); ++i) {
            auto &replacement = replacements[i];

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

            // Same or overlapping source range - combine the replacements
            std::get<0>(*buffer) = combinedRange;
            std::get<1>(*buffer) += std::get<1>(replacement);
            // If any replacement is a true replacement (not insertion), mark combined as replacement
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
        }

        REWRITE_LOG() << "  Applied " << replacements.size() << " replacements with priority sorting\n";
        REWRITE_LOG() << "=== END APPLYING ALL REPLACEMENTS ===\n\n";
    }
};

#endif // LLVM_REWRITE_GENERATORS_REPLACEMENTAPPLICATOR_H
