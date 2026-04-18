#define CATCH_CONFIG_MAIN

#if defined(__has_include)
#if __has_include(<catch2/catch_all.hpp>)
#include <catch2/catch_all.hpp>
#elif __has_include(<catch2/catch.hpp>)
#include <catch2/catch.hpp>
#else
#error "Catch2 main header not found"
#endif
#else
#include <catch2/catch.hpp>
#endif
