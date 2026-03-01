#pragma once

#if defined(__has_include)
#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#elif __has_include(<catch2/catch.hpp>)
#include <catch2/catch.hpp>
#else
#error "Catch2 headers not found"
#endif
#else
#include <catch2/catch_test_macros.hpp>
#endif

#if !defined(SKIP)
#define SKIP(msg)                                                               \
  do {                                                                          \
    INFO(msg);                                                                  \
    SUCCEED(msg);                                                               \
    return;                                                                     \
  } while (false)
#endif
