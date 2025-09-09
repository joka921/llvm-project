//
// Created by kalmbacj on 2025-09-09.
//

#include "./generator.h"

cppcoro::generator<int> gen() {
    co_yield 3;
    co_yield 4;
}