set(LLVM_LINK_COMPONENTS support)

add_clang_executable(rewrite-generators
        RewriteGenerators.cpp
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