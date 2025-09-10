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
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    _coro_storage<class std::vector<int, class std::allocator<int>>> v;
    _coro_storage<int> x;
  };

  class _detail_coro_statemachine_impl : public CoroImpl<_detail_coro_impl> {
  public:
    using CoroImpl<_detail_coro_impl>::CoroImpl; // Inherit constructors

    void run() {
      this->state.x.construct(3);; {
        this->state.v.construct({3, 2});;
        CO_YIELD(0, this->state.v.get()[1] + this->state.x.get());
        this->state.v.destroy();
      }
      CO_YIELD(1, this->state.x.get()-2);
      this->state.x.destroy();
    }
  };
}


/*
cppcoro::generator<int, float, Handle> gen() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    _coro_storage<class std::vector<int, class std::allocator<int>>> v;
    _coro_storage<int> x;
  };

  using Tp = cppcoro::generator<int, float, Handle>;
  COROUTINE_HEADER(Tp, _detail_coro_impl)
      this->state.x.construct(3);; {
        this->state.v.construct(std::initializer_list{3, 2});;
        CO_YIELD(1, this->state.v.get()[1] + this->state.x.get());
        this->state.v.destroy();
      }
      CO_YIELD(2, this->state.x.get()-2);
      this->state.x.destroy();
  COROUTINE_FOOTER
}
*/

