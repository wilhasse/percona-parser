#!/bin/bash
# Run the Go example with proper library path

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Set the library path to find libibd_reader.so
export LD_LIBRARY_PATH="$PROJECT_ROOT/build:$LD_LIBRARY_PATH"

# Run the example with all arguments passed through
"$SCRIPT_DIR/ibd-reader-example" "$@"