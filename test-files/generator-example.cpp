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
  std::vector<int> V = std::vector{3, 6, 9};
  int x;
  for (auto& x : V) {
    auto u = V;
    co_yield x;
  }
}
cppcoro::generator<int> gen() {
  {
    std::vector<int> V = std::vector{3, 6, 9};
    co_yield V[0];
  }
}

cppcoro::generator<int> gen(auto&& x) {
  {
    std::vector<int> V = std::vector{3, 6, 9};
    co_yield V[0] + x;
  }
}
cppcoro::generator<int> gen2(auto&& x) {
  {
    std::vector<int> V = std::vector{3, 6, 9};
    try {
    co_yield V[0] + x;
    } catch (...) {
    }
  }
}

auto lambda = [](int x) -> cppcoro::generator<int> {
  co_yield x;
}

class X {
 int v;

 void g();

 cppcoro::generator<int> gen() {
   auto x = v;
   co_yield x;
   co_yield v;

   g();
 }

};
*/

class X {
 int v;

 void g();

 cppcoro::generator<int, cppcoro::NoDetails, Handle> gen() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    // Member function 'this' pointer
    X* __self;

    // Local variables (including ranged-for loop variables)
    _coro_storage<int&, true> x;
  };

  using _ActualCoroType = cppcoro::generator<int, cppcoro::NoDetails, Handle>;
  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl)
      this->state.x.construct(__self->v);
      CO_YIELD(1, CO_GET(x));
      CO_YIELD(2, __self->v);

      __self->g();
      this->state.x.destroy();
  COROUTINE_FOOTER(this)
 }

};
