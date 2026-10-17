#pragma once

#include <cstdio>

#ifndef CLOG
  #if defined(STABLE)
    #define CLOG(fmt, ...) do { } while (0)
  #else
    #define CLOG(fmt, ...) do { std::fprintf(stdout, "[drv] " fmt "\n", ##__VA_ARGS__); } while (0)
  #endif
#endif
