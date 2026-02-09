//
// Command-line option definitions for the coroutine rewriter tool
// Main entry point is in Main.cpp
//

#include "llvm/Support/CommandLine.h"
#include "clang/Tooling/CommonOptionsParser.h"

using namespace llvm;
using namespace clang::tooling;

// Command-line options
llvm::cl::OptionCategory MyToolCategory("coroutine-rewriter");
cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
cl::extrahelp MoreHelp("\nRewrites C++20 coroutines to C++17 compatible state machines.\n");
