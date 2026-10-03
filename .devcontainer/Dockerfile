FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    clang-format \
    clang-tidy \
    clangd \
    cmake \
    curl \
    gdb \
    git \
    nginx \
    ninja-build \
    pkg-config \
    python3 \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# wrk2 is not packaged for Ubuntu; build it from source so benchmarks use
# fixed request-rate load generation (-R) instead of plain wrk, which
# undercounts tail latency under load due to coordinated omission.
RUN git clone --depth 1 https://github.com/giltene/wrk2.git /tmp/wrk2 \
    && make -C /tmp/wrk2 \
    && cp /tmp/wrk2/wrk /usr/local/bin/wrk2 \
    && rm -rf /tmp/wrk2

WORKDIR /workspace
