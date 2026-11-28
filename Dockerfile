# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" \
    && cmake --build build --target search_server search -j

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 ca-certificates && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home search_engine
COPY --from=build /src/build/search_server /usr/local/bin/search_server
COPY --from=build /src/build/search /usr/local/bin/search
COPY web/ /app/web/
USER search_engine
WORKDIR /app
ENTRYPOINT ["/usr/local/bin/search_server"]

# ThreadSanitizer stages. They follow `runtime` so that `target: runtime` of
# the search-server service is unaffected. CMAKE_BUILD_TYPE is intentionally
# not set: ENABLE_TSAN supplies -g -O1.
FROM ubuntu:24.04 AS tsan_build
RUN apt-get update && apt-get install -y --no-install-recommends \
    clang libclang-rt-18-dev make cmake git ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_CXX_COMPILER=clang++ \
    && cmake --build build-tsan --target index_stress_test -j

FROM tsan_build AS tsan
ENTRYPOINT ["/src/build-tsan/index_stress_test"]
