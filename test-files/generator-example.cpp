//
// Created by kalmbacj on 2025-09-09.
//

#include "./generator.h"
#include <vector>

/*
cppcoro::generator<int> gen() {
  int x = 3;
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
*/
cppcoro::generator<int, cppcoro::NoDetails, Handle> gen() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    // Local variables (including ranged-for loop variables)
    _coro_storage<int> x;
    _coro_storage<int> y;
    _coro_storage<class std::vector<int, class std::allocator<int>>> v;
    _coro_storage<decltype(v)> __range_0;
    _coro_storage<decltype(begin(std::declval<decltype(v)>()))> __begin_0;
    _coro_storage<decltype(end(std::declval<decltype(v)>()))> __end_0;
  };

  using _ActualCoroType = cppcoro::generator<int, cppcoro::NoDetails, Handle>;
  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl)
        this->state.x.construct(3);;
        this->state.x.get() += 2;
        this->state.y.construct(this->state.x.get());
        his->state.x.get(); {
          this->state.v.construct({3, 2});; {
            FOR_LOOP_HEADER(0)
            for (; this->state.__begin_0.get() != this->state.__end_0.get(); ++this->state.__begin_0.get()) {
              int &el = *this->state.__begin_0.get();

              co_yield el;
            }
          }
          FOR_LOOP_FOOTER(0)

          this->state.v.destroy();

          CO_YIELD(2, this->state.x.get()-2)
          this->state.y.destroy();
          this->state.x.destroy();
    COROUTINE_FOOTER
  }
