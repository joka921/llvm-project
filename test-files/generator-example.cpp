//
// Created by kalmbacj on 2025-09-09.
//

#include "./generator.h"
#include <vector>

template<typename T>
struct _coro_storage {
  alignas(T) char buffer[sizeof(T)];
  bool constructed = false;

  template<typename... Args>
  void construct(Args&&... args) {
    new(buffer) T(std::forward<Args>(args)...);
    constructed = true;
  }

  void destroy() {
    if (constructed) {
      reinterpret_cast<T*>(buffer)->~T();
      constructed = false;
    }
  }

  T& get() {
    return *reinterpret_cast<T*>(buffer);
  }

  const T& get() const {
    return *reinterpret_cast<const T*>(buffer);
  }

  ~_coro_storage() {
    destroy();
  }
};

cppcoro::generator<int> gen() {
  // Templated wrapper for manual object lifecycle management

  struct _detail_coro_impl {
    _coro_storage<class __gnu_cxx::__normal_iterator<int *, class std::vector<int, class std::allocator<int>>>> it;
    _coro_storage<class std::vector<int, class std::allocator<int>>> v;
    _coro_storage<int> x;
  } _coro_state;


    std::vector<int> v{3, 4};

    int x = 3;
    co_yield x;
  auto it = v.begin();
    co_yield *it;
}