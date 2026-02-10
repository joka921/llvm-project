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

cppcoro::generator<int> lambda () {
  int z = 3;
  auto lambda = [u = 4, &z] (auto x) { return u + z * x };

  co_yield lambda(3);
  co_yield lambda(5);
}
*/

cppcoro::generator<int, cppcoro::NoDetails, Handle> yieldTemporaries() {
  // _coro_storage and CoroImpl assumed to be available in global namespace
  struct _detail_coro_impl {
    // No local variables found in this coroutine

    // Subexpression temporaries
    _coro_storage<class std::basic_string<char, struct std::char_traits<char>, class std::allocator<char>>&, true> temp_1_0;
    _coro_storage<class std::basic_string<char, struct std::char_traits<char>, class std::allocator<char>>&, true> temp_1_1;

    // Buffer for yielded/awaited temporaries
    alignas(std::ranges::max(std::array{std::size_t{1}, alignof(int), alignof(int)})) char yieldBuffer[std::ranges::max(std::array{std::size_t{1}, sizeof(int), sizeof(int)})];

    // Destroy variables when coroutine is suspended at a specific state
    void destroySuspendedCoro(size_t curState) {
      switch (curState) {
        case 1:
          temp_1_0.destroy();
          temp_1_1.destroy();
          break;
        case 0:  // initial state
          break;
      }
    }
  };

  using _ActualCoroType = cppcoro::generator<int, cppcoro::NoDetails, Handle>;
  COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl) 

    CO_YIELD_BUFFERED((int), 1,  (CO_PAREN_INIT_OWNING(temp_1_0, std::string{"hallo"}) + CO_PAREN_INIT_OWNING(temp_1_1, std::string{"bye"})).size());

        this->state.temp_1_1.destroy();
        this->state.temp_1_0.destroy();
CO_RETURN_FALLOFF(2);
COROUTINE_FOOTER()
}

// ============================================================
// Test infrastructure: a simple task type that supports co_await
// and co_return with a value.
// ============================================================

struct SimpleTask {
    struct promise_type {
        int result = 0;

        SimpleTask get_return_object() noexcept {
            return SimpleTask{Handle<promise_type>::from_promise(*this)};
        }

        cppcoro::SuspendAlways initial_suspend() const noexcept { return {}; }
        cppcoro::SuspendAlways final_suspend() const noexcept { return {}; }
        void unhandled_exception() { std::terminate(); }
        void return_value(int v) { result = v; }
    };

    Handle<promise_type> handle;

    int run() {
        handle.resume();  // start (past initial_suspend)
        while (!handle.done()) {
            handle.resume();
        }
        int r = handle.promise().result;
        handle.destroy();
        return r;
    }
};

// ============================================================
// Test 1: CO_AWAIT_VOID — co_await on SuspendAlways (void result)
// ============================================================

SimpleTask testCoAwaitVoid() {
    struct _detail_coro_impl {
        _coro_storage<int&, true> x;
        std::vector<size_t> activeTryBlocks;

        void destroySuspendedCoro(size_t curState) {
            switch (curState) {
                case 2:
                case 1:
                    x.destroy();
                    break;
                case 0:
                    break;
            }
        }
    };

    using _ActualCoroType = SimpleTask;
    COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl)
    CO_PAREN_INIT_OWNING(x, 42);
    // co_await SuspendAlways{};
    CO_AWAIT_VOID(1, cppcoro::SuspendAlways{});
    // After resume, x should still be 42. Add 1 to prove we resumed.
    CO_GET(x) += 1;
    CO_RETURN_VALUE(2, CO_GET(x));
    this->state.x.destroy();
    CO_RETURN_VALUE_FALLOFF(3, 0);
    COROUTINE_FOOTER()
}

// ============================================================
// Test 2: CO_AWAIT_SUSPEND — co_await with non-void await_resume()
// ============================================================

struct IntAwaiter {
    int value;
    bool await_ready() const noexcept { return false; }
    void await_suspend(auto) const noexcept {}
    int await_resume() const noexcept { return value; }
};

SimpleTask testCoAwaitSuspend() {
    struct _detail_coro_impl {
        _coro_storage<int&, true> x;
        _coro_storage<IntAwaiter&, true> __awaiter_1;
        std::vector<size_t> activeTryBlocks;

        void destroySuspendedCoro(size_t curState) {
            switch (curState) {
                case 2:
                case 1:
                    __awaiter_1.destroy();
                    x.destroy();
                    break;
                case 0:
                    break;
            }
        }
    };

    using _ActualCoroType = SimpleTask;
    COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl)
    CO_PAREN_INIT_OWNING(x, 10);
    // auto result = co_await IntAwaiter{100};
    CO_AWAIT_SUSPEND(1, __awaiter_1, IntAwaiter{100});
    {
        auto result = CO_GET(__awaiter_1).await_resume();
        this->state.__awaiter_1.destroy();
        CO_GET(x) += result;
    }
    CO_RETURN_VALUE(2, CO_GET(x));
    this->state.x.destroy();
    CO_RETURN_VALUE_FALLOFF(3, 0);
    COROUTINE_FOOTER()
}

// ============================================================
// Test 3: Custom allocator
// ============================================================

static int allocCount = 0;
static int deallocCount = 0;

struct AllocTask {
    struct promise_type {
        int result = 0;

        static void* operator new(size_t size) {
            ++allocCount;
            return ::operator new(size);
        }

        static void operator delete(void* ptr, size_t) {
            ++deallocCount;
            ::operator delete(ptr);
        }

        AllocTask get_return_object() noexcept {
            return AllocTask{Handle<promise_type>::from_promise(*this)};
        }

        cppcoro::SuspendAlways initial_suspend() const noexcept { return {}; }
        cppcoro::SuspendAlways final_suspend() const noexcept { return {}; }
        void unhandled_exception() { std::terminate(); }
        void return_value(int v) { result = v; }
    };

    Handle<promise_type> handle;

    int run() {
        handle.resume();
        while (!handle.done()) {
            handle.resume();
        }
        int r = handle.promise().result;
        handle.destroy();
        return r;
    }
};

AllocTask testCustomAllocator() {
    struct _detail_coro_impl {
        std::vector<size_t> activeTryBlocks;

        void destroySuspendedCoro(size_t) {}
    };

    using _ActualCoroType = AllocTask;
    COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl)
    CO_RETURN_VALUE(1, 99);
    CO_RETURN_VALUE_FALLOFF(2, 0);
    COROUTINE_FOOTER()
}

// ============================================================
// Test 4: CO_RETURN_VALUE (also tested above, but standalone)
// ============================================================

SimpleTask testCoReturnValue() {
    struct _detail_coro_impl {
        _coro_storage<int&, true> a;
        _coro_storage<int&, true> b;
        std::vector<size_t> activeTryBlocks;

        void destroySuspendedCoro(size_t curState) {
            switch (curState) {
                case 1:
                    b.destroy();
                    a.destroy();
                    break;
                case 0:
                    break;
            }
        }
    };

    using _ActualCoroType = SimpleTask;
    COROUTINE_HEADER(_ActualCoroType, _detail_coro_impl)
    CO_PAREN_INIT_OWNING(a, 7);
    CO_PAREN_INIT_OWNING(b, 6);
    CO_RETURN_VALUE(1, CO_GET(a) * CO_GET(b));
    this->state.a.destroy();
    this->state.b.destroy();
    CO_RETURN_VALUE_FALLOFF(2, 0);
    COROUTINE_FOOTER()
}

// ============================================================
// Main — run all tests
// ============================================================

int main() {
    bool allPassed = true;
    auto check = [&](const char* name, bool cond) {
        if (!cond) {
            std::cout << "FAIL: " << name << std::endl;
            allPassed = false;
        } else {
            std::cout << "PASS: " << name << std::endl;
        }
    };

    // Existing test: testTryCatch
    // yields: 43 (y=x+1), 44 (z=y+1) x15 iterations, 42 (x)
    {
        /*
        std::vector<int> results;
        for (auto &i : testTryCatch()) {
            results.push_back(i);
        }
        bool ok = results.size() == 17 && results[0] == 43;
        for (size_t j = 1; j <= 15 && ok; ++j) ok = (results[j] == 44);
        ok = ok && results[16] == 42;
        check("testTryCatch yields [43, 44x15, 42]", ok);
        */
    }

    // Test 1: CO_AWAIT_VOID
    {
        int result = testCoAwaitVoid().run();
        check("testCoAwaitVoid returns 43", result == 43);
    }

    // Test 2: CO_AWAIT_SUSPEND (non-void await_resume)
    {
        int result = testCoAwaitSuspend().run();
        check("testCoAwaitSuspend returns 110", result == 110);
    }

    // Test 3: Custom allocator
    {
        allocCount = 0;
        deallocCount = 0;
        int result = testCustomAllocator().run();
        check("testCustomAllocator returns 99", result == 99);
        check("testCustomAllocator alloc called", allocCount == 1);
        check("testCustomAllocator dealloc called", deallocCount == 1);
    }

    // Test 4: CO_RETURN_VALUE
    {
        int result = testCoReturnValue().run();
        check("testCoReturnValue returns 42", result == 42);
    }

    if (allPassed) {
        std::cout << "\nAll tests passed!" << std::endl;
    } else {
        std::cout << "\nSome tests FAILED!" << std::endl;
        return 1;
    }
    return 0;
}
