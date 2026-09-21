#!/bin/sh
# Builds and runs the relay core unit tests, and builds the real-time bridge used by test/integration.js.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -Wall -Wextra -O1 relay_core_test.cpp -o /tmp/relay_core_test
/tmp/relay_core_test
g++ -std=c++17 -Wall -Wextra -O2 -pthread relay_cli.cpp -o /tmp/relay_cli
echo "built /tmp/relay_cli"
