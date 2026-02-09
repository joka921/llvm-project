set(LLVM_LINK_COMPONENTS support)

# Coroutine rewriter with modular structure:
# - infrastructure/: Generic reusable utilities
# - structures/: Data structure definitions
# - collectors/: Collector and rewriter classes
# - codegen/: Code generation for macro infrastructure
# See REFACTORING.md for details

add_clang_executable(rewrite-generators
        RewriteGenerators.cpp
        # Note: Header-only modular files are included via #include
        # Future .cpp files from refactoring will be added here
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