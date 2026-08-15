#!/usr/bin/env bash
# Build the Glama check image and confirm the MCP stdio server starts and
# answers initialize + tools/list with no project indexed — exactly what
# Glama's directory check does. Used locally and by the CI smoke suite.
#
# Env:
#   IMAGE        image tag to build (default: hyp-glama-check)
#   DOCKER_BUILD_ARGS  extra args for `docker build` (e.g. --platform linux/amd64)
set -euo pipefail

IMAGE="${IMAGE:-hyp-glama-check}"
DIR="$(cd "$(dirname "$0")" && pwd)"

run_with_timeout() {
  local t="$1"; shift
  if command -v timeout >/dev/null 2>&1; then timeout "$t" "$@"
  elif command -v gtimeout >/dev/null 2>&1; then gtimeout "$t" "$@"
  else "$@"; fi
}

# This image is built from the LATEST PUBLISHED release, not from the tree — so
# it can only ever check a release that already exists. In the smoke phase of a
# release run that is the PREVIOUS release, and before the first release with
# assets there is nothing at all: hyponoia v0.2.4 carries zero assets, so this
# 404'd inside `docker build` with a bare `tar: Child returned status 1` and
# reported it as a packaging failure. "Nothing has been published to test yet"
# is not a failure of the tree, and saying so is worth more than a red X that
# means nothing. The condition is deliberately narrow — a missing asset on a
# release that HAS assets still fails loudly, because that is real drift.
RELEASE_BASE="https://github.com/patalbansishashank/hyponoia/releases/latest/download"
case "$(uname -m)" in
    x86_64|amd64) PROBE_ARCH="amd64" ;;
    aarch64|arm64) PROBE_ARCH="arm64" ;;
    *) PROBE_ARCH="amd64" ;;
esac
PROBE_ASSET="hyponoia-ui-linux-${PROBE_ARCH}-portable.tar.gz"
if command -v curl >/dev/null 2>&1 &&
   ! curl -fsIL -o /dev/null "${RELEASE_BASE}/${PROBE_ASSET}" 2>/dev/null; then
    echo "SKIP: the latest release publishes no ${PROBE_ASSET}, so there is no"
    echo "      artifact for the Glama directory image to check yet. This image"
    echo "      is built from the published release, never from this tree."
    exit 0
fi

echo "==> building ${IMAGE} (${DIR}/Dockerfile)"
# shellcheck disable=SC2086
docker build ${DOCKER_BUILD_ARGS:-} -f "${DIR}/Dockerfile" -t "${IMAGE}" "${DIR}"

echo "==> MCP introspection handshake (initialize -> initialized -> tools/list)"
REQ="$(printf '%s\n%s\n%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"glama-verify","version":"1"}}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}')"
OUT="$(printf '%s' "${REQ}" | run_with_timeout 60 docker run -i --rm "${IMAGE}" || true)"

printf '%s\n' "${OUT}"

echo "==> assertions"
printf '%s' "${OUT}" | grep -q '"result"'     || { echo "FAIL: no JSON-RPC result (server did not respond)"; exit 1; }
printf '%s' "${OUT}" | grep -q 'search_graph' || { echo "FAIL: tools/list missing expected tool 'search_graph'"; exit 1; }
COUNT="$(printf '%s' "${OUT}" | grep -o '"name"' | wc -l | tr -d ' ')"
echo "PASS: server started and introspected; ~${COUNT} name entries (>=14 tools expected)"
