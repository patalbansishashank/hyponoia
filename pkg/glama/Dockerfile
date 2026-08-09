# Glama MCP directory (glama.ai) check image — NOT required to run the tool.
#
# hyponoia needs NO Docker to run. This image exists only so Glama
# can build a sandbox, launch the stdio MCP server, and run its introspection
# checks, which power the directory's score badge. The same image is exercised
# locally and in CI by pkg/glama/verify.sh to guard against drift.
#
# We pull the "-portable" release asset, which is fully statically linked
# (gcc -static) — unlike the standard asset, which dynamically links glibc/
# libstdc++ and would fail on an older base. Because it's static, the runtime
# base image is arbitrary. TARGETARCH is amd64 / arm64, matching the asset names.

FROM debian:bookworm-slim AS fetch
ARG TARGETARCH
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && curl -fsSL "https://github.com/patalbansishashank/hyponoia/releases/latest/download/hyponoia-linux-${TARGETARCH}-portable.tar.gz" \
      | tar -xz -C /tmp hyponoia hyp-integrations.json LICENSE THIRD_PARTY_NOTICES.md \
 && chmod +x /tmp/hyponoia \
 && rm -rf /var/lib/apt/lists/*

FROM debian:bookworm-slim
COPY --from=fetch /tmp/hyponoia /usr/local/bin/hyponoia
COPY --from=fetch /tmp/hyp-integrations.json /usr/local/share/hyponoia/hyp-integrations.json
COPY --from=fetch /tmp/LICENSE /tmp/THIRD_PARTY_NOTICES.md /usr/share/doc/hyponoia/
ENV HYP_CACHE_DIR=/tmp/hyp
ENTRYPOINT ["hyponoia"]
