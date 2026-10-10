FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Ubuntu 24.04 pins this toolchain to LLVM 18; CI installs the same package
# set so clang-format/clang-tidy results match between local and CI runs.
# ca-certificates: needed for HTTPS git clones (wrk2, GoogleTest FetchContent).
# libclang-rt-18-dev: ASan/UBSan runtimes, not pulled in without recommends.
# libssl-dev: required to build wrk2.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    clang \
    clang-format \
    clang-tidy \
    clangd \
    cmake \
    curl \
    gdb \
    git \
    libclang-rt-18-dev \
    libssl-dev \
    netcat-openbsd \
    nginx \
    ninja-build \
    pkg-config \
    python3 \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# wrk2 is not packaged for Ubuntu; build it from source so benchmarks use
# fixed request-rate load generation (-R) instead of plain wrk, which
# undercounts tail latency under load due to coordinated omission.
#
# wrk2 is unmaintained and only builds on x86_64 as-is. To also build on
# arm64 (e.g. Apple Silicon hosts), with both repositories pinned to a
# commit for reproducible images:
# - Replace the bundled LuaJIT 2.0 (no arm64 support) with LuaJIT 2.1,
#   which renamed the luaL_reg type to luaL_Reg.
# - Drop the unused x86-only <x86intrin.h> include.
# - Store getopt_long()'s return value in an int: char is unsigned on
#   arm64, so comparing it with -1 never matches and argument parsing
#   always fails.
ARG WRK2_COMMIT=44a94c17d8e6a0bac8559b53da76848e430cb7a7
ARG LUAJIT_COMMIT=c6ffc141a8762b41703f9287d63d93622a13dd8f
RUN git clone https://github.com/giltene/wrk2.git /tmp/wrk2 \
    && git -C /tmp/wrk2 checkout --quiet "${WRK2_COMMIT}" \
    && rm -rf /tmp/wrk2/deps/luajit \
    && git clone https://github.com/LuaJIT/LuaJIT.git /tmp/wrk2/deps/luajit \
    && git -C /tmp/wrk2/deps/luajit checkout --quiet "${LUAJIT_COMMIT}" \
    && sed -i 's/\bluaL_reg\b/luaL_Reg/g' /tmp/wrk2/src/script.c \
    && sed -i '/#include <x86intrin.h>/d' /tmp/wrk2/src/hdr_histogram.c \
    && sed -i 's/^    char c, \*\*header = headers;/    int c; char **header = headers;/' /tmp/wrk2/src/wrk.c \
    && grep -q 'int c; char \*\*header = headers;' /tmp/wrk2/src/wrk.c \
    && make -C /tmp/wrk2 -j"$(nproc)" \
    && cp /tmp/wrk2/wrk /usr/local/bin/wrk2 \
    && rm -rf /tmp/wrk2

WORKDIR /workspace
