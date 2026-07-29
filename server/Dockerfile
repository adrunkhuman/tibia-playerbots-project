# syntax=docker/dockerfile:1

FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        build-essential \
        ccache \
        cmake \
        libboost-date-time-dev \
        libboost-iostreams-dev \
        libboost-system-dev \
        libcrypto++-dev \
        libfmt-dev \
        libluajit-5.1-dev \
        libmysqlclient-dev \
        libpugixml-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CCACHE_SLOPPINESS=pch_defines,time_macros

WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake ./cmake
COPY src ./src

RUN --mount=type=cache,target=/root/.cache/ccache \
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_FLAGS=-fpch-preprocess \
        -DSKIP_GIT=ON \
        -DUSE_LUAJIT=ON \
    && cmake --build build --parallel 2 \
    && ccache --show-stats

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        libboost-date-time-dev \
        libboost-iostreams-dev \
        libboost-system-dev \
        libcrypto++-dev \
        libfmt-dev \
        libluajit-5.1-dev \
        libmysqlclient-dev \
        libpugixml-dev \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 angelion

WORKDIR /app
COPY --from=builder /src/build/tfs ./tfs
COPY --chown=angelion:angelion config.lua key.pem ./
COPY --chown=angelion:angelion data ./data

EXPOSE 7171 7172

USER angelion

CMD ["./tfs"]
