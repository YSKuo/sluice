# Builds the sluice release binary and packages it alone in a slim runtime
# image for the benchmark stack. Build context: repository root.

FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang-18 \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY src/ src/
# Tests are skipped: they need GoogleTest from the network and are already
# run by scripts/check.sh and CI.
RUN cmake --preset release -DSLUICE_BUILD_TESTS=OFF \
    && cmake --build --preset release

FROM ubuntu:24.04

COPY --from=build /src/build/release/sluice /usr/local/bin/sluice

EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/sluice"]
# Routes are required; docker-compose.yml supplies them.
CMD ["--help"]
