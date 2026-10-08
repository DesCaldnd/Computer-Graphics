#pragma once

// The harness is public (oxwald/net/net_harness.hpp); tests keep using the ox::net::test names.
#include <oxwald/net/net_harness.hpp>

namespace ox::net::test {
using ox::net::MemoryHarness;
using ox::net::pumpReal;
} // namespace ox::net::test
