# rewrite-generators - C++20 Coroutine to C++17 State Machine Rewriter

## Project Overview

This is a Clang LibTooling-based source-to-source transformation tool that rewrites C++20 coroutines into C++17-compatible state machines. The tool performs AST-level analysis and generates macro-based state machine implementations that preserve the semantics of the original coroutine code.

**Location**: `clang-tools-extra/rewrite-generators` within the LLVM monorepo
**LLVM Project Root**: `/local/data-ssd/kalmbacj/llvm-project`

## Current Directory Structure

```
rewrite-generators/
├── CMakeLists.txt              # Build configuration
├── RewriteGenerators.cpp       # Main implementation (~3940 lines)
├── Helpers.h                   # AST manipulation utilities
└── LocalVariableCollector.h    # Visitor for local variable collection
```

### Key Files

- **RewriteGenerators.cpp**: Contains the main rewriting logic with three primary classes:
  - `CoroutineRewriter`: Top-level AST visitor that identifies coroutine functions
  - `CoroutineBodyRewriter`: Nested visitor that transforms coroutine body statements
  - `CoroutineRewriterFrontendAction`: Frontend action that orchestrates the rewriting

- **Helpers.h**: Utility functions including:
  - `unwrapExpr()`: Strips implicit AST nodes to access original expressions
  - `getSourceText()`: Extracts source code text from AST nodes
  - `isPrValue()`: Determines if an expression is a prvalue
  - `typeAsString()`: Generates fully qualified type names
  - `getTypeForAutoRefRefVariable()`: Computes types for forwarding references

- **LocalVariableCollector.h**: RecursiveASTVisitor subclass that:
  - Collects all local variables declared in coroutine bodies
  - Tracks variable names, types, and reference semantics
  - Detects name collisions across coroutine scopes
  - Determines storage types vs. reference types for state machine variables

## Architecture & Key Data Structures

### Core Data Structures

```cpp
struct CoroutineInfo {
    const FunctionDecl *function;
    std::set<LocalVariable> localVariables;
    std::vector<RangedForLoop> rangedForLoops;
    std::vector<TryCatchBlock> tryCatchBlocks;
    std::vector<CoroutineStatement> coroutineStatements;
    std::vector<FunctionParameter> parameters;
    bool isMemberFunction;
    bool isConstMemberFunction;
    bool isLambda;
    // ... more fields
};

struct CoroutineStatement {
    enum Type { YIELD, AWAIT, RETURN };
    Type type;
    const Stmt *stmt;
    const Expr *operand;
    unsigned index;
    bool needsBuffering;  // For temporaries
    QualType bufferType;
    std::vector<std::string> aliveVariables;
    std::vector<TemporaryInfo> temporaries;
};

struct LocalVariable {
    std::string name;
    std::string type;           // Original type (may be reference)
    bool isOwning;              // Whether storage owns the object
    std::string referenceType;  // Type for access
    bool isReference;           // True if original is a reference
    SourceLocation location;
    int priority;               // File position for ordering
};
```

### Processing Flow

1. **AST Traversal**: `CoroutineRewriter` traverses the translation unit
2. **Coroutine Detection**: Identifies functions containing coroutine keywords
3. **Analysis Phase**:
   - Collects local variables via `LocalVariableCollector`
   - Identifies suspension points (co_await, co_yield, co_return)
   - Analyzes ranged-for loops and try-catch blocks
   - Tracks variable lifetimes and temporaries
4. **Code Generation**:
   - Generates state machine structure with `COROUTINE_HEADER` macro
   - Transforms variable declarations to use `CO_GET()`, `CO_BRACED_INIT()`, etc.
   - Rewrites suspension points to state machine transitions
   - Adds variable lifetime management (construct/destroy calls)
   - Inserts `COROUTINE_FOOTER` macro at function end

## Parent Directory Context

### LLVM Project Structure

The tool is part of the LLVM monorepo and has access to the full Clang and LLVM infrastructure:

```
/local/data-ssd/kalmbacj/llvm-project/
├── clang/                      # Clang compiler
│   └── include/clang/          # Clang headers
│       ├── AST/                # AST node definitions
│       ├── ASTMatchers/        # Pattern matching for AST
│       ├── Basic/              # SourceLocation, diagnostics, etc.
│       ├── Frontend/           # Compiler frontend infrastructure
│       ├── Lex/                # Lexer and preprocessor
│       ├── Rewrite/            # Source rewriting utilities
│       └── Tooling/            # LibTooling infrastructure
├── llvm/                       # LLVM infrastructure
│   └── include/llvm/           # LLVM headers
│       ├── ADT/                # Data structures (StringRef, etc.)
│       ├── Support/            # CommandLine, ErrorHandling, etc.
│       └── IR/                 # LLVM IR definitions
├── clang-tools-extra/          # Extra Clang tools (our location)
│   ├── rewrite-generators/     # ← This tool
│   ├── backport-starts-with/   # Similar backport tool
│   ├── backport-using-enum/    # Similar backport tool
│   ├── clang-tidy/             # Linting framework
│   └── ...
└── build/                      # Build output directory
```

### Related Tools

This tool follows the same pattern as other backport tools in `clang-tools-extra/`:
- `backport-starts-with`: Backports C++20 `.starts_with()` to older standards
- `backport-using-enum`: Backports C++20 using-enum declarations
- `backport-defaulted-equality`: Backports defaulted comparison operators

## File Access Configuration

**Claude has read access to all files within**: `/local/data-ssd/kalmbacj/llvm-project`

This includes:
- All source files in `rewrite-generators/`
- Clang headers in `clang/include/clang/`
- LLVM headers in `llvm/include/llvm/`
- Other tools in `clang-tools-extra/`
- Build configuration files
- Test files in `test-files/` (at project root)

When analyzing code, feel free to read any relevant header files or related tools for context.

## Build Configuration

**Build System**: CMake
**Executable Name**: `rewrite-generators`

**Dependencies** (from CMakeLists.txt):
- `LLVM_LINK_COMPONENTS`: support
- `clangAST`: AST node definitions and traversal
- `clangASTMatchers`: Pattern matching for AST queries
- `clangBasic`: SourceManager, diagnostics, language options
- `clangFrontend`: Compiler frontend infrastructure
- `clangSerialization`: AST serialization (for PCH support)
- `clangTooling`: LibTooling framework for standalone tools

**Build Instructions**: Typically built as part of the full LLVM build system.

## Transformation Strategy

### Macro-Based Code Generation

The tool generates code using the following macros (defined elsewhere):

- `COROUTINE_HEADER(...)`: Declares state machine structure and setup
- `COROUTINE_FOOTER(...)`: Cleanup and state machine finalization
- `COROUTINE_FOOTER_WITH_TRY(...)`: Footer variant for try-catch support
- `CO_GET(var)`: Accesses a variable from the state machine state
- `CO_BRACED_INIT(var, ...)`: Initializes a non-owning variable with braced init
- `CO_BRACED_INIT_OWNING(var, ...)`: Initializes an owning variable with braced init
- `CO_PAREN_INIT(var, ...)`: Initializes a non-owning variable with parentheses
- `CO_PAREN_INIT_OWNING(var, ...)`: Initializes an owning variable with parentheses
- `CO_RETURN_FALLOFF(index)`: Handles implicit co_return at function end

### Variable Lifetime Management

Variables declared in the coroutine are stored in a state machine structure. The tool:

1. **Determines Storage Type**:
   - For prvalue initializers: owning storage (owns the object)
   - For lvalue/xvalue references: non-owning storage (stores reference)

2. **Tracks Variable Lifetimes**:
   - Records which variables are alive at each suspension point
   - Generates destructor calls in reverse declaration order
   - Handles scope-based destruction at block exits

3. **Manages References**:
   - Preserves reference semantics (lvalue refs vs rvalue refs)
   - Uses `std::add_lvalue_reference_t` or `std::add_rvalue_reference_t` as needed
   - Properly forwards prvalues as rvalue references

### Suspension Point Transformation

Each `co_await`, `co_yield`, or `co_return` becomes:
1. A state index assignment
2. Destruction of out-of-scope variables
3. A control flow jump (via macro)
4. (On resume) Construction of newly-in-scope variables

### Special Handling

- **Ranged-for loops**: Desugared into manual iterator loops
- **Try-catch blocks**: Tracked for special footer macro
- **Member functions**: `this` pointer is captured and forwarded
- **Lambdas**: Handled with special member function treatment
- **Temporaries**: Lifetime-extended using buffer variables when needed

## Current State

**Git Branch**: `coroutine-rewrite-second-try`

Recent commits focus on:
- Yielding correct buffer types
- Temporary materialization handling
- Buffered rewrite implementation
- `co_return` support

## Notes for Claude

- The codebase uses extensive AST traversal and manipulation via RecursiveASTVisitor
- Source code transformations are performed using Clang's Rewriter class
- The tool maintains detailed debug logging via `REWRITE_LOG()` macro (can be toggled)
- Type analysis is complex due to references, auto, and temporary materialization
- The transformation must preserve exact C++ semantics including value categories
