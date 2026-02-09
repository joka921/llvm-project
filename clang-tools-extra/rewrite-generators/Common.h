//
// Common types and utilities shared across the coroutine rewriter
//

#ifndef LLVM_REWRITE_GENERATORS_COMMON_H
#define LLVM_REWRITE_GENERATORS_COMMON_H

#include "clang/Basic/SourceLocation.h"
#include <iostream>
#include <streambuf>
#include <string>
#include <tuple>

using namespace clang;

// Null stream for logging control
class NullBuffer : public std::streambuf {
protected:
    int overflow(int c) override { return traits_type::not_eof(c); }
};

inline std::ostream& null_stream() {
    static NullBuffer nullBuffer;
    static std::ostream nullStream(&nullBuffer);
    return nullStream;
}

// Logging macro for fine-grained control
// Toggle between null_stream() and std::cout to enable/disable logging
#define REWRITE_LOG() std::cout
//#define REWRITE_LOG() null_stream()

// Replacement types
// The bool means `true` for replace, `false` for insert after the beginning
using Replacement = std::tuple<SourceRange, std::string, bool>;

// Global replacement with priority (SourceRange, replacement_string, priority, isReplace)
using PrioritizedReplacement = std::tuple<SourceRange, std::string, int, bool>;

#endif // LLVM_REWRITE_GENERATORS_COMMON_H
