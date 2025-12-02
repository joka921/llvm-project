set(LLVM_LINK_COMPONENTS support)

add_clang_executable(backport-starts-with
        BackportStartsWith.cpp
)
target_link_libraries(backport-starts-with
        PRIVATE
        clangAST
        clangASTMatchers
        clangBasic
        clangFrontend
        clangSerialization
        clangTooling
)