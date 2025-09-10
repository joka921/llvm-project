//
// Created by kalmbacj on 2025-09-09.
//

#include "./generator.h"
#include <vector>

cppcoro::generator<int> gen() {
  struct _detail_coro_impl {
    struct _detail_coro_impl _coro_state{};
    class __gnu_cxx::__normal_iterator<int *, class std::vector<int, class std::allocator<int>>> it{};
    class std::vector<int, class std::allocator<int>> v{};
    int x{};
  } _coro_state;


    std::vector<int> v{3, 4};

    int x = 3;
    co_yield x;
  auto it = v.begin();
    co_yield *it;
}