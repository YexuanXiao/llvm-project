// Benchmarks the stock generator (generator.hpp): exceptions rethrow through
// every nested coroutine frame.
#include "generator.hpp"

#define GENERATOR_IMPL_NAME "generator.hpp (stock, rethrow per frame)"
#include "bench_core.h"
