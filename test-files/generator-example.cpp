//
// Created by kalmbacj on 2025-09-09.
//

#include "./generator.h"
#include <vector>

/*
cppcoro::generator<int> gen() {
  int x = 3;
  auto&& y = 12;
  auto&& y2 = y;
  auto z = 13;
  //x+=2;
  //int y = x;
  {
    std::vector<int> v{3, 2};
    for (auto& el : v) {
      co_yield el;
   }
   }
  co_yield x-2;
}

cppcoro::generator<int> gen() {
  int&& x = 3;
  int&& y = std::move(x);
  co_yield x-2;
}
*/
cppcoro::generator<int, cppcoro::NoDetails, Handle> gen() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    // Local variables (including ranged-for loop variables)
    _coro_storage<std::remove_reference_t<int &&>, int &&> x;
    _coro_storage<std::add_pointer_t<std::remove_reference_t<int &&>>, int &&> y;
  };

  using _ActualCoroType = cppcoro::generator<int, cppcoro::NoDetails, Handle>;
  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl) 
  this->state.x.construct(3);;
  this->state.y.construct(std::move(this->state.x.get().ref_));;
  CO_YIELD(1,  this->state.x.get()-2)
    this->state.y.destroy();
    this->state.x.destroy();
COROUTINE_FOOTER
}
