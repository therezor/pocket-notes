#!/bin/sh
# Build and run the host parity test (needs a C++17 compiler). Run from the repo root.
set -e
c++ -O2 -std=c++17 -Wall -Ifirmware/components/tinydecide/src \
  host/engine_test.cpp firmware/components/tinydecide/src/engine.cpp -o /tmp/td_engine_test 2>&1
/tmp/td_engine_test .
