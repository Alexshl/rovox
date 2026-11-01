# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors

# ---- base: libraries needed at run time ------------------------------------
FROM debian:bookworm-slim AS base
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libasound2 libatomic1 libfreetype6 libfontconfig1 \
      libebur128-1 libmp3lame0 libcpp-httplib0.11 libopus0 libtomlplusplus3 \
 && rm -rf /var/lib/apt/lists/*

# ---- web: the browser UI (TypeScript, Vite); unit tests run here, the bundle is copied into the runtime image -----------
FROM node:22-bookworm-slim AS web
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY web/ ./
RUN npm test && npm run build

# ---- toolchain: compilers and -dev packages --------------------------------
FROM base AS toolchain
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config git ca-certificates ccache \
      libasound2-dev libfreetype-dev libfontconfig1-dev libopus-dev \
      nlohmann-json3-dev libcli11-dev libcpp-httplib-dev libebur128-dev libmp3lame-dev libtomlplusplus-dev \
      libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxi-dev \
 && rm -rf /var/lib/apt/lists/*
# Tracktion's nested JUCE submodule is declared with an ssh URL (git@github.com:...). Rewrite it to https so that
# anonymous clones / CI / `git submodule update` inside the container work without a fork. See README "Building".
RUN git config --system url."https://github.com/".insteadOf "git@github.com:"
ENV CCACHE_DIR=/ccache CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache

# ---- build: configure, compile, test ---------------------------------------
FROM toolchain AS build
ARG ROVOX_GIT_SHA=unknown
ARG ROVOX_BUILD_SPIKES=OFF
# Optional: cap compiler parallelism (Tracktion/JUCE translation units need ~1.5 GB each; 15 jobs OOM a 12 GB Docker VM)
ARG ROVOX_BUILD_JOBS=
ENV ROVOX_GIT_SHA=${ROVOX_GIT_SHA}
# open LV2 plugins from Debian for the plugin tests (instruments that play MIDI, effects); the tests fail, not skip, when they are missing
RUN apt-get update \
 && apt-get install -y --no-install-recommends calf-plugins zynaddsubfx-lv2 lsp-plugins-lv2 x42-plugins avldrums.lv2 avldrums.lv2-soundfont \
 && rm -rf /var/lib/apt/lists/*
ENV ROVOX_REQUIRE_LV2=1
WORKDIR /src
COPY . .
RUN --mount=type=cache,target=/ccache \
    cmake -S . -B build/linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DROVOX_BUILD_SPIKES=${ROVOX_BUILD_SPIKES} \
 && cmake --build build/linux-release ${ROVOX_BUILD_JOBS:+--parallel ${ROVOX_BUILD_JOBS}} \
 && ctest --test-dir build/linux-release --output-on-failure \
 && mkdir -p /out/bin \
 && cp build/linux-release/src/app/rovox_artefacts/Release/rovox /out/bin/ \
 && if [ "${ROVOX_BUILD_SPIKES}" = "ON" ]; then \
      cp build/linux-release/spikes/p1_headless/p1_headless_artefacts/Release/p1_headless /out/bin/ \
      && cp build/linux-release/spikes/rovox_spikes_artefacts/Release/rovox_spikes /out/bin/; fi \
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
# The open LV2 pack (task 400/620): instruments and effects the agent can load without anything mounted. Debian packages, GPL / LGPL, installed
# from the distribution and not part of the Rovox source (see NOTICE.md). Mount your own plugins at /plugins. python3 runs the rovox-skills sidecar
# and the end-to-end scripts.
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 calf-plugins zynaddsubfx-lv2 lsp-plugins-lv2 x42-plugins avldrums.lv2 avldrums.lv2-soundfont \
 && rm -rf /var/lib/apt/lists/*
COPY skills/rovox_skills /opt/rovox-skills/rovox_skills
COPY tools/m45_e2e.py /opt/rovox-skills/m45_e2e.py
ENV PYTHONPATH=/opt/rovox-skills
COPY --from=build /out/bin/rovox /usr/local/bin/rovox
COPY --from=web /web/dist /usr/share/rovox/web
USER 10001
ENV ROVOX_DATA_DIR=/data
# `rovox serve --bind 0.0.0.0` without a token generates one and prints it (task 350); set ROVOX_TOKEN to choose your own
ENV ROVOX_GENERATE_TOKEN=1
VOLUME /data
EXPOSE 4810
ENTRYPOINT ["rovox"]
CMD ["--version"]

# ---- spike-p1: runtime + the headless spike (build with ROVOX_BUILD_SPIKES=ON)
FROM runtime AS spike-p1
COPY --from=build /out/bin/p1_headless /usr/local/bin/p1_headless
ENTRYPOINT ["p1_headless"]
CMD []

# ---- spikes: runtime + P1..P5 spike binaries + open LV2 plugins from Debian bookworm (ROVOX_BUILD_SPIKES=ON)
FROM runtime AS spikes
USER root
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libopus0 python3 \
      x42-plugins lsp-plugins-lv2 zynaddsubfx-lv2 avldrums.lv2 avldrums.lv2-soundfont calf-plugins \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/bin/rovox_spikes /usr/local/bin/rovox_spikes
COPY spikes/p3_browser_stream /opt/rovox/spikes/p3_browser_stream
COPY spikes/p4_mcp /opt/rovox/spikes/p4_mcp
USER 10001
ENTRYPOINT ["rovox_spikes"]
CMD []

# ---- skills: the rovox-skills sidecar alone (Python, standard library only); talks to the core over MCP
FROM python:3.12-slim-bookworm AS skills
COPY skills /opt/rovox-skills
RUN pip install --no-cache-dir /opt/rovox-skills
USER 10001
ENV ROVOX_URL=http://rovox:4810/mcp
ENTRYPOINT ["rovox-skills"]
CMD ["list"]
