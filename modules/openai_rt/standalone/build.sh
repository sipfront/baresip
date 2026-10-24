#!/usr/bin/env bash
# Builds sf-voiceai in a Debian 12 container (matching the agent image) and puts the binary into
# build/sf-voiceai under the repo root. Needs docker with BuildKit (docker 23+).
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
mkdir -p "${REPO}/build"
DOCKER_BUILDKIT=1 docker build -f "${REPO}/modules/openai_rt/standalone/Dockerfile" \
    --target out --output "type=local,dest=${REPO}/build" "${REPO}"
ls -la "${REPO}/build/sf-voiceai"
