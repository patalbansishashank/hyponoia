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
#
# The name carries "-ui-" because that is the ONLY variant the release builds:
# every package-release.sh call in _build.yml passes --variant ui, and
# `hyponoia-linux-<arch>-portable.tar.gz` has never been published. This asked
# for a name that does not exist and 404'd, which nobody could see because the
# only release this fork had cut carried zero assets.
#
# Extract the whole archive rather than naming members: the UI pack is
# content-addressed (hyp-ui-<sha256>.pack), so it cannot be listed literally.

FROM debian:bookworm-slim AS fetch
ARG TARGETARCH
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && curl -fsSL "https://github.com/patalbansishashank/hyponoia/releases/latest/download/hyponoia-ui-linux-${TARGETARCH}-portable.tar.gz" \
      | tar -xz -C /tmp \
 && chmod +x /tmp/hyponoia \
 && rm -rf /var/lib/apt/lists/*

FROM debian:bookworm-slim
COPY --from=fetch /tmp/hyponoia /usr/local/bin/hyponoia
COPY --from=fetch /tmp/hyp-integrations.json /usr/local/share/hyponoia/hyp-integrations.json
COPY --from=fetch /tmp/LICENSE /tmp/THIRD_PARTY_NOTICES.md /usr/share/doc/hyponoia/
ENV HYP_CACHE_DIR=/tmp/hyp
ENTRYPOINT ["hyponoia"]
