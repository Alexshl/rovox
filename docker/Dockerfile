# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

# ---- base: libraries needed at run time ------------------------------------
FROM debian:bookworm-slim AS base
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libasound2 libatomic1 libfreetype6 libfontconfig1 \
 && rm -rf /var/lib/apt/lists/*

# ---- toolchain: compilers and -dev packages --------------------------------
FROM base AS toolchain
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config git ca-certificates ccache \
      libasound2-dev libfreetype-dev libfontconfig1-dev \
      libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxi-dev \
 && rm -rf /var/lib/apt/lists/*
ENV CCACHE_DIR=/ccache CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache

# ---- build: configure, compile, test ---------------------------------------
FROM toolchain AS build
ARG ROVOX_GIT_SHA=unknown
ARG ROVOX_BUILD_SPIKES=OFF
ENV ROVOX_GIT_SHA=${ROVOX_GIT_SHA}
WORKDIR /src
COPY . .
RUN --mount=type=cache,target=/ccache \
    cmake -S . -B build/linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DROVOX_BUILD_SPIKES=${ROVOX_BUILD_SPIKES} \
 && cmake --build build/linux-release \
 && ctest --test-dir build/linux-release --output-on-failure \
 && mkdir -p /out/bin \
 && cp build/linux-release/src/app/rovox_artefacts/Release/rovox /out/bin/ \
 && if [ "${ROVOX_BUILD_SPIKES}" = "ON" ]; then \
      cp build/linux-release/spikes/p1_headless/p1_headless_artefacts/Release/p1_headless /out/bin/; fi \
 && strip /out/bin/*

# ---- dev: interactive work via bind mount ----------------------------------
FROM toolchain AS dev
RUN apt-get update \
 && apt-get install -y --no-install-recommends gdb valgrind strace python3 sox \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src

# ---- runtime: minimal image ------------------------------------------------
FROM base AS runtime
RUN useradd --uid 10001 --create-home --home-dir /home/rovox --shell /usr/sbin/nologin rovox \
 && mkdir -p /data && chown 10001:10001 /data
COPY --from=build /out/bin/rovox /usr/local/bin/rovox
USER 10001
VOLUME /data
EXPOSE 4810
ENTRYPOINT ["rovox"]
CMD ["--version"]

# ---- spike-p1: runtime + the headless spike (build with ROVOX_BUILD_SPIKES=ON)
FROM runtime AS spike-p1
COPY --from=build /out/bin/p1_headless /usr/local/bin/p1_headless
ENTRYPOINT ["p1_headless"]
CMD []
