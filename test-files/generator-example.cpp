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
*/

cppcoro::generator<int, cppcoro::NoDetails, Handle> gen() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    // Local variables (including ranged-for loop variables)
    _coro_storage<class std::vector<int, class std::allocator<int>>&, true> V;
    _coro_storage<int&, true> x;
    _coro_storage<int &, false> x;
    _coro_storage<std::add_lvalue_reference_t<class std::vector<int, class std::allocator<int>>> &, false> __range_0;
    _coro_storage<std::decay_t<decltype(std::declval<std::add_lvalue_reference_t<class std::vector<int, class std::allocator<int>>> &>().begin())> &, true> __begin_0;
    _coro_storage<std::decay_t<decltype(std::declval<std::add_lvalue_reference_t<class std::vector<int, class std::allocator<int>>> &>().end())> &, true> __end_0;
    _coro_storage<int &, false> x;
    _coro_storage<class std::vector<int, class std::allocator<int>>&, true> u;
  };

  using _ActualCoroType = cppcoro::generator<int, cppcoro::NoDetails, Handle>;
  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl)
        this->state.V.construct(std::vector{3, 6, 9});
        this->state.x.construct();;
        this->state.__range_0.construct(V);
        this->state.__begin_0.construct(CO_GET(__range_0).begin());
        this->state.__end_0.construct(CO_GET(__range_0).end());
        for (; CO_GET(__begin_0) != CO_GET(__end_0); ++CO_GET(__begin_0)) {
          this->state.x.construct(*CO_GET(__begin_0));
          this->state.u.construct(CO_GET(V));
          CO_YIELD(1, CO_GET(x))
          this->state.u.destroy();
          this->state.x.destroy();
        }
        this->state.__end_0.destroy();
        this->state.__begin_0.destroy();
        this->state.__range_0.destroy();
    }

    this->state.x.destroy();
    this->state.V.destroy();
COROUTINE_FOOTER

}
}

