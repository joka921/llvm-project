//
// Offset-based replacement engine for string-based source rewriting
// (Clang-independent — works on plain std::string buffers)
//

#ifndef LLVM_REWRITE_GENERATORS_STRINGREPLACEMENTAPPLICATOR_H
#define LLVM_REWRITE_GENERATORS_STRINGREPLACEMENTAPPLICATOR_H

#include "clang/Basic/SourceManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Lex/Lexer.h"
#include "../Common.h"
#include <algorithm>
#include <string>
#include <vector>

using namespace clang;

struct OffsetReplacement {
    unsigned offset;    // byte offset into source string
    unsigned length;    // bytes to replace (0 = pure insertion)
    std::string text;   // replacement text
    int priority;       // ordering at same position
};

class StringReplacementApplicator {
private:
    std::vector<OffsetReplacement> replacements_;

public:
    void addReplacement(unsigned offset, unsigned length,
                        const std::string &text, int priority) {
        replacements_.push_back({offset, length, text, priority});
    }

    void addReplacement(const OffsetReplacement &r) {
        replacements_.push_back(r);
    }

    void clear() {
        replacements_.clear();
    }

    /// Apply all collected replacements to the source string.
    /// Mirrors ReplacementApplicator::applyReplacements() logic:
    /// - Sort by offset descending, then priority ascending
    /// - Deduplicate identical entries
    /// - Merge overlapping/adjacent at same offset (concatenate text)
    /// - Apply from back to front so earlier offsets stay valid
    std::string applyTo(const std::string &source) const {
        if (replacements_.empty()) return source;

        // Work on a copy for sorting
        auto sorted = replacements_;

        // Sort by offset descending, then priority ascending
        std::stable_sort(sorted.begin(), sorted.end(),
            [](const OffsetReplacement &a, const OffsetReplacement &b) {
                if (a.offset != b.offset)
                    return a.offset > b.offset; // Reverse order for back-to-front
                if (a.priority != b.priority)
                    return a.priority < b.priority;
                return a.text < b.text;
            });

        // Deduplicate exact duplicates
        sorted.erase(std::unique(sorted.begin(), sorted.end(),
            [](const OffsetReplacement &a, const OffsetReplacement &b) {
                return a.offset == b.offset && a.length == b.length &&
                       a.text == b.text && a.priority == b.priority;
            }), sorted.end());

        // Apply from back to front, merging at same offset
        std::string result = source;
        size_t i = 0;
        while (i < sorted.size()) {
            unsigned offset = sorted[i].offset;
            unsigned length = sorted[i].length;
            std::string combinedText = sorted[i].text;

            // Merge all replacements at the same offset
            size_t j = i + 1;
            while (j < sorted.size() && sorted[j].offset == offset) {
                combinedText += sorted[j].text;
                // Use the wider length
                if (sorted[j].length > length)
                    length = sorted[j].length;
                j++;
            }

            // Clamp to source bounds
            if (offset > result.size()) {
                i = j;
                continue;
            }
            if (offset + length > result.size())
                length = result.size() - offset;

            REWRITE_LOG() << "  StringRepl: offset=" << offset << " len=" << length
                          << " text='" << combinedText.substr(0, 60) << "'"
                          << (combinedText.size() > 60 ? "..." : "") << "\n";

            result.replace(offset, length, combinedText);
            i = j;
        }

        return result;
    }

    /// Convert a SourceRange-based PrioritizedReplacement to an offset-based one.
    /// @param range       The source range of the replacement
    /// @param text        The replacement text
    /// @param priority    Priority for ordering
    /// @param isReplace   true = replace range, false = insert at range.getBegin()
    /// @param SM          Source manager
    /// @param langOpts    Language options
    /// @param baseFileOffset  File offset of the template source start (subtracted from all offsets)
    static OffsetReplacement fromSourceRange(
        const SourceRange &range, const std::string &text,
        int priority, bool isReplace,
        const SourceManager &SM, const LangOptions &langOpts, unsigned baseFileOffset) {

        OffsetReplacement result;
        result.offset = SM.getFileOffset(range.getBegin()) - baseFileOffset;
        result.text = text;
        result.priority = priority;

        if (isReplace) {
            // Compute length: from begin to end-of-token at range.getEnd()
            unsigned endOffset = SM.getFileOffset(
                Lexer::getLocForEndOfToken(range.getEnd(), 0, SM, langOpts));
            unsigned beginOffset = SM.getFileOffset(range.getBegin());
            result.length = endOffset - beginOffset;
        } else {
            result.length = 0; // Pure insertion
        }

        return result;
    }
};

#endif // LLVM_REWRITE_GENERATORS_STRINGREPLACEMENTAPPLICATOR_H
