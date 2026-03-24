# Multi-stage Dockerfile for building LLVM with clang++ and BackportUsingEnum tool
# Stage 1: Build environment with all dependencies
FROM ubuntu:22.04 AS builder

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    python3 \
    python3-pip \
    libz-dev \
    zlib1g-dev \
    libxml2-dev \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /build

# Clone the LLVM project from the current git repository
# This avoids copying the entire context and uses git to fetch only tracked files
RUN git init llvm-project

# Copy only the git repository metadata and configuration
COPY .git /build/llvm-project/.git

# Checkout the current branch/commit
RUN cd /build/llvm-project && \
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
    ../llvm-project/llvm

# Build only the required targets:
# - clang++ (part of the clang target)
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
FROM ubuntu:22.04 AS runtime

# Install only runtime dependencies
RUN apt-get update && apt-get install -y \
    libstdc++6 \
    libz3-4 \
    zlib1g \
    libxml2 \
    && rm -rf /var/lib/apt/lists/*

# Copy the installed LLVM binaries and libraries from builder
COPY --from=builder /opt/llvm /opt/llvm

# Add LLVM binaries to PATH
ENV PATH="/opt/llvm/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/llvm/lib:${LD_LIBRARY_PATH}"

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Add additional repositories
RUN apt-get update && \
    apt-get install -y software-properties-common wget && \
    add-apt-repository -y ppa:mhier/libboost-latest && \
    rm -rf /var/lib/apt/lists/*

# Add Kitware repository for latest CMake
RUN wget -O kitware-archive.sh https://apt.kitware.com/kitware-archive.sh && \
    chmod +x kitware-archive.sh && \
    ./kitware-archive.sh && \
    rm kitware-archive.sh

# Install development tools and libraries
RUN apt-get update && \
    apt-get install -y \
    build-essential \
    cmake \
    libicu-dev \
    tzdata \
    pkg-config \
    uuid-runtime \
    uuid-dev \
    git \
    libjemalloc-dev \
    ninja-build \
    libzstd-dev \
    libssl-dev \
    libboost1.81-dev \
    libboost-program-options1.81-dev \
    libboost-iostreams1.81-dev \
    libboost-url1.81-dev  \
    libboost-container1.81-dev 

RUN rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace

# Default command
CMD ["/bin/bash"]
