# Coroutine Rewriter Refactoring Plan

## Completed Steps

### Phase 1: Infrastructure & Data Structures ✓

1. **Created directory structure:**
   - `infrastructure/` - Generic, reusable Clang/LLVM utilities
   - `structures/` - Data structure definitions grouped semantically
   - `collectors/` - Collector and rewriter visitor classes
   - `codegen/` - Code generation for coroutine macro infrastructure

2. **Created common utilities:**
   - `Common.h` - Shared types (Replacement, PrioritizedReplacement), logging macros
   - `infrastructure/ASTHelpers.h` - Generic AST utilities (unwrapExpr, isPrValue, typeAsString, etc.)
   - `infrastructure/ReplacementApplicator.h` - Generic replacement application with priority handling

3. **Extracted data structures:**
   - `structures/CoroutineStructures.h` - CoroutineInfo, CoroutineStatement, TemporaryInfo, FunctionParameter, LocalVariable
   - `structures/LoopStructures.h` - RangedForLoop
   - `structures/TryCatchStructures.h` - TryCatchBlock
   - `structures/ScopeStructures.h` - ScopeInfo, ScopeEndReplacement

4. **Moved existing collectors:**
   - `collectors/LocalVariableCollector.h` - Variable declaration collection
   - `collectors/HelperCollectors.h` - TemporaryCollector, VariableReferenceCollector

5. **Extracted code generation:**
   - `codegen/MacroCodeGenerator.h` - makeStateGetCall, makeBracedInitPrefix, etc.

## Remaining Work

### Phase 2: Large Visitor Class Extraction

The `CoroutineBodyRewriter` class (~2400 lines) needs to be split into multiple single-responsibility classes:

#### collectors/VariableRewriter.h/cpp
**Responsibility:** Rewrite variable declarations and references
- Extract initialization form detection (getInitializationForm, handleBracedInitialization, handleParenthesizedInitialization)
- Extract variable declaration rewriting (VisitDeclStmt logic)
- Extract variable reference rewriting (using VariableReferenceCollector)
- Extract scope tracking for variable lifetime
- Needs: scope stack, variable names set, decl/ref replacements

#### collectors/LoopRewriter.h/cpp
**Responsibility:** Rewrite loops and control flow
- Extract ranged-for loop collection and rewriting
- Extract break/continue statement handling
- Extract loop variable destructor insertion
- Needs: loop stack, scope info, variable info

#### collectors/TryCatchRewriter.h/cpp
**Responsibility:** Rewrite try-catch blocks
- Extract try-catch collection
- Extract catch clause transformation
- Extract try block variable tracking
- Needs: try block stack, scope info, variable info

#### collectors/CoroutineStatementRewriter.h/cpp
**Responsibility:** Rewrite coroutine keywords
- Extract co_await/co_yield/co_return collection
- Extract operand transformation
- Extract temporary buffer handling
- Extract alive variable tracking
- Needs: all variable/scope info from previous collectors

### Phase 3: Orchestration

#### CoroutineRewriter.h/cpp (refactor existing)
**Current:** Contains both collection and main orchestration
**Target:** Pure orchestrator that:
- Coordinates all collectors/rewriters
- Passes state between them in correct order
- Handles function signature rewriting
- Generates state struct definitions
- Manages overall rewrite flow

#### Main.cpp (extract from RewriteGenerators.cpp)
- Move main(), FrontendAction, ASTConsumer to separate file
- Keep only rewriting logic in remaining files

### Phase 4: Scope Tracking Infrastructure

#### infrastructure/ScopeTrackingVisitor.h/cpp (optional enhancement)
Base class providing scope tracking for all visitors:
- CompoundStmt tracking (outside-in)
- Scope stack management
- Loop nesting tracking
- Try-block nesting tracking

This would reduce duplication across visitor classes.

## Architecture

```
Collection Phase (reads AST):
  LocalVariableCollector → variables
  CoroutineStatementCollector → coroutine statements

Rewriting Phase (generates replacements):
  VariableRewriter(variables) → decl/ref replacements
  LoopRewriter(variables, scopes) → loop replacements
  TryCatchRewriter(variables, scopes) → try-catch replacements
  CoroutineStatementRewriter(all info) → coroutine replacements

Application Phase:
  ReplacementApplicator(all replacements) → applies to Rewriter
```

## Benefits of This Structure

1. **Separation of Concerns:** Each class has a single, clear responsibility
2. **Reusability:** Infrastructure components work with any Clang tool
3. **Testability:** Each collector/rewriter can be tested independently
4. **Maintainability:** Changes to one feature don't affect others
5. **Clarity:** Dataflow is explicit (collectors → rewriters → applicator)

## Migration Strategy

1. ✓ Create new file structure with common utilities
2. Update RewriteGenerators.cpp to use new headers (next step)
3. Extract VariableRewriter incrementally
4. Extract LoopRewriter incrementally
5. Extract TryCatchRewriter incrementally
6. Extract CoroutineStatementRewriter incrementally
7. Refactor CoroutineRewriter as pure orchestrator
8. Extract Main.cpp
9. Test at each step
10. Remove old code once all functionality is migrated
