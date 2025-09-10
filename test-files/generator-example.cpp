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
            this->state.x.construct(3); {
                this->state.v.construct({3, 2});;
                co_yield  this->state.v.get()[1] + this->state.x.get();
                this->state.v.destroy();
            }
            co_yield
            this->state.x.get() - 2;
            this->state.x.destroy();
        }
    };
}
