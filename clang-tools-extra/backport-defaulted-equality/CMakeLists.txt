set(LLVM_LINK_COMPONENTS support)

add_clang_executable(backport-defaulted-equality
        BackportDefaultedEquality.cpp
)
target_link_libraries(backport-defaulted-equality
        PRIVATE
        clangAST
        clangASTMatchers
        clangBasic
        clangFrontend
        clangSerialization
        clangTooling
)