#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

echo "=== Building TruMinus Host Tests ==="

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake
echo "Configuring..."
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Build
echo "Building..."
cmake --build . --parallel

echo ""
echo "=== Build complete ==="
echo "Run tests with: $BUILD_DIR/tests"
echo "Or with CTest: cd $BUILD_DIR && ctest --output-on-failure"
