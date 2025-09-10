//
// Created by kalmbacj on 2025-09-09.
//

#include "./generator.h"
#include <vector>

/*
cppcoro::generator<int> gen() {
  int x = 3;
  std::vector<int>v{3, 4} ;
  std::vector<int>w(3, 4) ;
  std::vector<int>a = {3, 4} ;
  auto it = v.begin();
  co_return;
}
cppcoro::generator<int> gen() {
  int x = 3;
  std::vector<int>v{3, 4} ;
  std::vector<int>w(3, 4) ;
  std::vector<int>a = {3, 4} ;
  auto it = v.begin();
  co_return;
}
*/

cppcoro::generator<int> gen() {
  // _coro_storage struct assumed to be available in global namespace
  struct _detail_coro_impl {
    _coro_storage<class std::vector<int, class std::allocator<int>>> a;
    _coro_storage<class __gnu_cxx::__normal_iterator<int *, class std::vector<int, class std::allocator<int>>>> it;
    _coro_storage<class std::vector<int, class std::allocator<int>>> v;
    _coro_storage<class std::vector<int, class std::allocator<int>>> w;
    _coro_storage<int> x;
  } _coro_state;


  _coro_state.x.construct(3);;
  _coro_state.v.construct({3, 4}); ;
  _coro_state.w.construct(w(3, 4)); ;
  _coro_state.a.construct({3, 4}); ;
  _coro_state.it.construct(_coro_state.v.get().begin());;
  co_return;
}
