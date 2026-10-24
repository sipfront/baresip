# Builds sf-voiceai for the Sipfront agent image: Debian 12, libre 3.23.0 (the agent image
# ships libre.so.34.23.0 in /usr/local/lib), libwebsockets/json-c/libpulse from Debian.
#   docker build -f modules/openai_rt/standalone/Dockerfile -t sf-voiceai-build .   (repo root)
# The binary is /out/sf-voiceai in the image; build.sh copies it out.
FROM debian:bookworm-slim AS build
ARG RE_VERSION=v3.23.0
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config \
        libssl-dev zlib1g-dev libwebsockets-dev libjson-c-dev libpulse-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch ${RE_VERSION} https://github.com/baresip/re.git /re \
    && cmake -S /re -B /re/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /re/build -j"$(nproc)" && cmake --install /re/build && ldconfig
COPY include /src/include
COPY modules/openai_rt /src/modules/openai_rt
RUN cmake -S /src/modules/openai_rt/standalone -B /build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build -j"$(nproc)" \
    && mkdir -p /out && cp /build/sf-voiceai /out/

# a scratch stage so `docker build --output` can export just the binary
FROM scratch AS out
COPY --from=build /out/sf-voiceai /sf-voiceai
