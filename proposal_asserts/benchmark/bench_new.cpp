// Benchmarks the improved generator (generator_improved.hpp): exceptions
// travel along the cooperative-cancellation chain; intermediate frames
// neither throw nor catch.
#include "generator_improved.hpp"

#define GENERATOR_IMPL_NAME "generator_improved.hpp (cancellation chain)"
#include "bench_core.h"
