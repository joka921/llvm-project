set(CMAKE_CXX_STANDARD 23)
add_library(testlib joka-examples.cpp generator-example.cpp
  using-enum-example.cpp)

add_executable(generator-example generator-example.cpp)
add_executable(generator-example-unfolded generator-example-unfolded.cpp)
