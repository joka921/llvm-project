//
// Created by kalmbacj on 2025-09-09.
//

#include "./generator.h"
#include <vector>

/*
cppcoro::generator<int> gen() {
  int x = 3;
  {
    std::vector<int> v{3, 2};
    co_yield v[1] + x;
  }
  co_yield x-2;
}
*/

cppcoro::generator<int> gen() {
  // _coro_storage struct assumed to be available in global namespace
  struct _detail_coro_impl {
    _coro_storage<class std::vector<int, class std::allocator<int>>> v;
    _coro_storage<int> x;
  } _coro_state;


  _coro_state.x.construct(3);;
  {
    _coro_state.v.construct(std::initializer_list{3, 2});
    co_yield _coro_state.v.get()[1] + _coro_state.x.get();
    _coro_state.v.destroy();
}
  co_yield _coro_state.x.get()-2;
  _coro_state.x.destroy();
}
