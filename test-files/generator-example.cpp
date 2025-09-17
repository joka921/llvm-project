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
*/

auto lambda = [](int x) -> cppcoro::generator<int> {
  co_yield x;
};
