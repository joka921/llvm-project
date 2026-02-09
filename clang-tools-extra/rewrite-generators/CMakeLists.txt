set(LLVM_LINK_COMPONENTS support)

# Coroutine rewriter with modular structure:
# - infrastructure/: Generic reusable utilities
# - structures/: Data structure definitions
# - collectors/: Collector and rewriter classes
# - codegen/: Code generation for macro infrastructure
# See REFACTORING.md for details

add_clang_executable(rewrite-generators
        Main.cpp
        RewriteGenerators.cpp
        # Note: CoroutineBodyRewriter.cpp and CoroutineRewriter.cpp are included via #include
        # to maintain current compilation structure. These can be compiled separately in future.
)

target_link_libraries(rewrite-generators
        PRIVATE
        clangAST
        clangASTMatchers
        clangBasic
        clangFrontend
        clangSerialization
        clangTooling
)