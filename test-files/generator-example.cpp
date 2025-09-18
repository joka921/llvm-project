//
// Created by kalmbacj on 2025-09-09.
//

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
  co_yield 4;
}

auto lambda = [](int x) -> cppcoro::generator<int> {
 auto y = x + 2;
 co_yield y;
}
*/

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
