set(LLVM_LINK_COMPONENTS support)

add_clang_executable(backport-using-enum
        BackportUsingEnum.cpp
)
target_link_libraries(backport-using-enum
        PRIVATE
        clangAST
        clangASTMatchers
        clangBasic
        clangFrontend
        clangSerialization
        clangTooling
)