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
    Y(X) {
    }
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

cppcoro::generator<int> yieldTemporaries() {

    co_yield (std::string{"hallo"}).size();
}

cppcoro::generator<int> lambdaAsMember () {
    int z = 3;
    auto lambda = [u = 4, &z] (auto x) { return u + z * x; };

    co_yield lambda(3);
    co_yield lambda(5);
}

cppcoro::generator<int> gen() {
  std::vector<int> V = std::vector{3, 6, 9};
  int z;
  for (auto& x : V) {
    auto u = V;
    co_yield x;
  }
  co_yield z;
}
cppcoro::generator<int> gen(int a, int b) {
  const auto& c = a * b;
  const auto& d = b;
  co_yield c * 3;
  co_yield d + 3;
}
*/

cppcoro::generator<int, cppcoro::NoDetails, Handle> testTryCatch() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  // tryBlockParent: {kNoTryBlock}
  using PromiseType = cppcoro::generator<int, cppcoro::NoDetails, Handle>::promise_type;
  struct GeneratorStateMachine : CoroImpl<GeneratorStateMachine, PromiseType> {
    // Local variables (including ranged-for loop variables)
    _coro_storage<int &, true> x;
    _coro_storage<int &, true> y;
    _coro_storage<int &, true> i;
    _coro_storage<int &, true> z;

    // Constructed flags for local variables
    struct {
      bool x = false;
      bool y = false;
      bool i = false;
      bool z = false;
    } __constructed;

    // Awaiter storage members
    _coro_storage<decltype(std::declval<PromiseType &>().initial_suspend()) &, true> __initial_awaiter;
    _coro_storage<decltype(std::declval<PromiseType &>().final_suspend()) &, true> __final_awaiter;
    _coro_storage<struct cppcoro::SuspendAlways &, true> __awaiter_1;
    _coro_storage<struct cppcoro::SuspendAlways &, true> __awaiter_2;
    _coro_storage<struct cppcoro::SuspendAlways &, true> __awaiter_3;

    void destroyAllConstructed() {
      DESTROY_IF_CONSTRUCTED(z);
      DESTROY_IF_CONSTRUCTED(i);
      DESTROY_IF_CONSTRUCTED(y);
      DESTROY_IF_CONSTRUCTED(x);
      DESTROY_IF_CONSTRUCTED();
      DESTROY_IF_CONSTRUCTED(__awaiter_3);
      DESTROY_IF_CONSTRUCTED(__awaiter_2);
      DESTROY_IF_CONSTRUCTED(__awaiter_1);
    }

    void handleUnhandledException() {
      destroyAllConstructed();
      promise().unhandled_exception();
      CO_RETURN_IMPL_IMPL(__final_awaiter, 0);
    }

    // Destroy variables when coroutine is suspended at a specific state
    void destroySuspendedCoro(size_t curState) {
      switch (curState) {
        case 5:
        cleanup_4:
        case 4:
          break;
        cleanup_3:
        case 3:
          __awaiter_3.destroy();
          break;
        cleanup_2:
        case 2:
          __awaiter_2.destroy();
          z.destroy();
          i.destroy();
          break;
        cleanup_1:
        case 1:
          __awaiter_1.destroy();
          y.destroy();
          x.destroy();
          break;
        case 0: // initial state - initial awaiter is alive
          __initial_awaiter.destroy();
          break;
      }
    }

    void doStepImpl() {
      switch (this->curState) {
        case 0: break;
        case 1: goto resume_try_0;
        case 2: goto resume_try_0;
        case 3: goto label_3;
        default: return;
      }
      __initial_awaiter.get().ref_.await_resume();
      __initial_awaiter.destroy();

      CO_INIT(x, ( 42));
    resume_try_0:
      try {
        try {
          switch (this->curState) {
            case 1: goto label_1;
            case 2: goto label_2;
            default: break;
          }

          CO_INIT(y, ( CO_GET(x) + 1));
          CO_YIELD(1, __awaiter_1, CO_GET(y));
          CO_INIT(i, ( 15));
          while (CO_GET(i)) {
            CO_INIT(z, ( CO_GET(y) + 1));
            CO_YIELD(2, __awaiter_2, CO_GET(z));
            --CO_GET(i);
            DESTROY_UNCONDITIONALLY(z);
          }
          DESTROY_UNCONDITIONALLY(i);
          DESTROY_UNCONDITIONALLY(y);
        }
      } catch (...) {
        DESTROY_IF_CONSTRUCTED(y);
        DESTROY_IF_CONSTRUCTED(__awaiter_1);
        DESTROY_IF_CONSTRUCTED(i);
        DESTROY_IF_CONSTRUCTED(z);
        DESTROY_IF_CONSTRUCTED(__awaiter_2);
        throw;
      } catch (std::exception &e) {
        std::cout << "Caught exception: " << e.what() << " x=" << CO_GET(x) << std::endl;
      } catch (...) {
        std::cout << "Caught unknown exception, x=" << CO_GET(x) << std::endl;
      }
      CO_YIELD(3, __awaiter_3, CO_GET(x));
      CO_RETURN_VOID(4, __final_awaiter);
    }
  };
  return GeneratorStateMachine::ramp();
}
