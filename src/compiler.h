#pragma once

// No standard attribute keeps a function out of line.
#if defined(_MSC_VER) && !defined(__clang__)
#define UEE_NOINLINE __declspec(noinline)
#define UEE_COLD
#else
#define UEE_NOINLINE [[gnu::noinline]]
#define UEE_COLD [[gnu::cold]]
#endif

#define UEE_MAYBE_UNUSED [[maybe_unused]]
