//
// Created by kalmbacj on 2025-09-09.
//

#include <algorithm>
#include <iostream>
#include <ostream>
#include <ranges>

#include "./generator.h"
#include <vector>

struct X {
  int i;
};

struct Y {
  Y(X) {}
};
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
    std::vector<int> v1 = {3, 6, 9};
    std::vector<int> v2 {3, 6, 9};
    std::vector<int> v3(6, 9);
    auto v4 = std::vector<int>{3, 7};
    co_yield v1[0];
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

cppcoro::generator<int> gen() {
  {
    std::vector<int> v1 = {3, 6, 9};
    std::vector<int> v2 {3, 6, 9};
    std::vector<int> v3(6, 9);
    auto v4 = std::vector<int>{3, 7};
    co_yield v1[0];
  }
}

cppcoro::generator<const int> gen() {
int i = 0;
  while (true) {
  const int& x = {3};
  if (i < 4) {
    continue;
  } else {
    break;
  }
  co_yield x;
  ++i;
  }
}


cppcoro::generator<int> conversion() {
  Y y = X{3};
  Y& z = y;
  int a = 3;
  int b(a);
  int& c(b);
  int& d{c};
  co_yield 4;
}

auto lambda = [](int x) -> cppcoro::generator<int> {
 auto y = x + 2;
 co_yield y;
}

cppcoro::generator<int> temporaries() {
  int a = 4;
  co_yield 4;
  co_yield 4.3;
  co_yield a;
  co_yield a + 2;
}

cppcoro::generator<int> testTryCatch() {
  int x = 42;
  try {
    int y = x + 1;
    co_yield y;
    int i = 15;
    while (i) {
      int z = y + 1;
      co_yield z;
      --i;
    }
  } catch (std::exception& e) {
    std::cout << "Caught exception: " << e.what() << " x=" << x << std::endl;
  } catch (...) {
    std::cout << "Caught unknown exception, x=" << x << std::endl;
  }
  co_yield x;
  co_return;
}

cppcoro::generator<int> testSuspendedDestroy() {
  int x = 42;
  co_yield x;
  {
    int y = x + 1;
    co_yield y;
    int i = 15;
    while (i) {
      int z = y + 1;
      co_yield z;
      --i;
    }
  }
  {
  int z2;
  co_yield x;
  }
  co_return;
}

cppcoro::generator<int> yieldTemporaries() {

  co_yield (std::string{"hallo"} + std::string{"bye"}).size();
}
*/

cppcoro::generator<int, cppcoro::NoDetails, Handle> testTryCatch() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    // Local variables (including ranged-for loop variables)
    _coro_storage<int&, true> x;
    _coro_storage<int&, true> y;
    _coro_storage<int&, true> i;
    _coro_storage<int&, true> z;

    // Exception handling infrastructure
    std::vector<size_t> activeTryBlocks;

    void handleException(std::exception_ptr eptr, size_t& nextState, std::function<void()> resume) {
        destroyBecauseOfExceptionHandling(activeTryBlocks.back());
      nextState = dispatchExceptionHandling(std::move(eptr));
      resume();
    }

    size_t dispatchExceptionHandling(std::exception_ptr eptr) {
      switch (activeTryBlocks.back()) {
        case 0: return catchClauseImpl_0(std::move(eptr));
        default: std::terminate();
      }
    }

    // Exception handler member functions
    size_t catchClauseImpl_0(std::exception_ptr eptr) {
      auto nextState = activeTryBlocks.back();
      activeTryBlocks.pop_back();
      auto lambda = [&]() {
        try {
          std::rethrow_exception(eptr);
        } catch (class std::exception & e) {
    std::cout << "Caught exception: " << e.what() << " x=" << CO_GET_STATE(x) << std::endl;
  } catch (...) {
    std::cout << "Caught unknown exception, x=" << CO_GET_STATE(x) << std::endl;
  } 
        return nextState;
      };
      if (activeTryBlocks.empty()) {
        return lambda();
      } else {
        try {
          return lambda();
        } catch (...) {
          return dispatchExceptionHandling(std::current_exception());
        }
      }
    }

    // Destroy variables in case of exception in try block
    void destroyBecauseOfException(size_t tryCatchBlockIndex) {
      switch (tryCatchBlockIndex) {
        case 0:
          if (y.constructed) { y.destroy(); }
          if (i.constructed) { i.destroy(); }
          if (z.constructed) { z.destroy(); }
          break;
        default: break;
      }
    }

    // Destroy variables when coroutine is suspended at a specific state
    void destroySuspendedCoro(size_t curState) {
      switch (curState) {
        case 4:
        cleanup_3:
        case 3:
          break;
        cleanup_2:
        case 2:
          z.destroy();
          i.destroy();
        cleanup_1:
        case 1:
          y.destroy();
          x.destroy();
          break;
        case 0:  // initial state
          break;
      }
    }
  };

  using _ActualCoroType = cppcoro::generator<int, cppcoro::NoDetails, Handle>;
  COROUTINE_HEADER_WITH_TRY(_ActualCoroType, _detail_coro_impl) 
  CO_PAREN_INIT_OWNING(x,  42);
  TRY_BEGIN(18446744073709551615ULL); {
    CO_PAREN_INIT_OWNING(y,  CO_GET(x) + 1);
    CO_YIELD(1,  CO_GET(y));
    CO_PAREN_INIT_OWNING(i,  15);
    while (CO_GET(i)) {
      CO_PAREN_INIT_OWNING(z,  CO_GET(y) + 1);
      CO_YIELD(2,  CO_GET(z));
      --CO_GET(i);
        this->state.z.destroy();
}
      this->state.i.destroy();
    this->state.y.destroy();
} TRY_END(18446744073709551615ULL);
  CO_YIELD(3,  CO_GET(x));
  CO_RETURN_VOID(4);
    this->state.x.destroy();
CO_RETURN_FALLOFF(5);
COROUTINE_FOOTER_WITH_TRY()
}

int main() {
  for (auto &i : testTryCatch()) {
    std::cout << i << std::endl;
  }
}
