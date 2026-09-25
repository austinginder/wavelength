# Linux build environment for scripts/build-release.sh. Ubuntu 22.04 (glibc 2.35) so the binary
# runs on current distributions; libstdc++ is linked statically.
FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ make git ca-certificates zlib1g-dev python3 wget \
    && rm -rf /var/lib/apt/lists/*
RUN wget -qO- https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-$(uname -m).tar.gz \
      | tar xz -C /opt && ln -s /opt/cmake-3.31.6-linux-$(uname -m)/bin/* /usr/local/bin/
