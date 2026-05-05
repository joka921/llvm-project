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

cl::opt<bool> InPlace("i",
    cl::desc("Rewrite the input file in-place"),
    cl::cat(MyToolCategory));

cl::opt<std::string> OutputFile("o",
    cl::desc("Write rewritten output to <file> (only valid with a single input file)"),
    cl::value_desc("file"),
    cl::cat(MyToolCategory));
