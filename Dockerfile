# `wavelength mcp` from the latest release, for MCP directories that start a server to list its
# tools (Glama). No plugins inside: songs play the built-in synth, drums and effects, and the
# fallbacks of their tracks. The engine's own Linux checks use scripts/docker/.
FROM debian:bookworm-slim
ARG TARGETARCH
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
 && a=$([ "$TARGETARCH" = arm64 ] && echo arm64 || echo x86_64) && f=wavelength-linux-$a.tar.gz \
 && cd /tmp && curl -fsSLO "https://github.com/austinginder/wavelength/releases/latest/download/$f" \
 && curl -fsSLO https://github.com/austinginder/wavelength/releases/latest/download/SHA256SUMS.txt \
 && grep "  $f\$" SHA256SUMS.txt | sha256sum -c - \
 && tar -xzf "$f" -C /opt && ln -s "/opt/wavelength-linux-$a/wavelength" /usr/local/bin/wavelength \
 && rm "$f" SHA256SUMS.txt && apt-get purge -y curl && apt-get autoremove -y && rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["wavelength", "mcp"]
