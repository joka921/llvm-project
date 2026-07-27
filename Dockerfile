# Multi-stage Dockerfile for building clang++ and the backport-using-enum tool
# Stage 1: Build environment with all dependencies
FROM ubuntu:26.04 AS builder

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    python3 \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Clone the LLVM project from the current git repository
# This avoids copying the entire context and uses git to fetch only tracked files
RUN git init llvm-project

# Copy only the git repository metadata and configuration
COPY .git /build/llvm-project/.git

# Checkout the current branch/commit
RUN git config --global --add safe.directory /build/llvm-project && \
    cd /build/llvm-project && \
    git reset --hard HEAD

# Create build directory
RUN mkdir -p /build/build

# Configure LLVM with CMake
# Only enable the required projects (clang and clang-tools-extra)
# Use Release build type for optimized binaries
# Enable only the targets needed (adjust as needed - using native here)
RUN cd /build/build && \
    cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/llvm \
    -DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
    -DLLVM_TARGETS_TO_BUILD="X86" \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_DOXYGEN=OFF \
    -DLLVM_ENABLE_SPHINX=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_OPTIMIZED_TABLEGEN=ON \
    -DLLVM_BUILD_TOOLS=ON \
    -DLLVM_ENABLE_RTTI=ON \
    -DLLVM_ENABLE_EH=ON \
    -DLLVM_ENABLE_LIBXML2=OFF \
    ../llvm-project/llvm

# Build only the required targets:
# - clang++ (part of the clang target, also provides the builtin headers the
#   tool needs to parse translation units)
# - backport-using-enum (the custom tool)
RUN cd /build/build && \
    ninja clang && \
    ninja backport-using-enum

# Install the built binaries to the prefix
RUN cd /build/build && \
    ninja install

# Also manually copy the backport-using-enum tool if it wasn't installed
RUN if [ -f /build/build/bin/backport-using-enum ]; then \
        cp /build/build/bin/backport-using-enum /opt/llvm/bin/; \
    fi

# Stage 2: Minimal runtime environment
FROM ubuntu:26.04 AS runtime

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install only the runtime dependencies of clang++/backport-using-enum.
# libstdc++-15-dev is needed for its headers: without them the tool cannot parse
# any translation unit that includes a standard header. The version suffix
# follows the GCC in the base image and has to be bumped along with it.
RUN apt-get update && apt-get install -y \
    libstdc++6 \
    zlib1g \
    libstdc++-15-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy the installed LLVM binaries and libraries from builder
COPY --from=builder /opt/llvm /opt/llvm

# Add LLVM binaries to PATH (clang++ and backport-using-enum live there)
ENV PATH="/opt/llvm/bin:${PATH}"

# Set working directory
WORKDIR /workspace

# Default command
CMD ["/bin/bash"]

# Keep this as the very last instruction: pointing the loader at /opt/llvm/lib
# earlier would make every subsequent RUN (apt-get, shell tooling, ...) pick up
# LLVM's libraries instead of the system ones.
ENV LD_LIBRARY_PATH="/opt/llvm/lib:${LD_LIBRARY_PATH}"
