# Runs the Windows build under Wine for scripts/build-release.sh's smoke test.
FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends wine64 python3 \
    && rm -rf /var/lib/apt/lists/*
ENV WINEDEBUG=-all WINEPREFIX=/wine PATH=/usr/lib/wine:$PATH
RUN wine64 wineboot --init 2>/dev/null; true
