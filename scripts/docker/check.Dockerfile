# Linux environment for scripts/check-linux.sh: the build tools of linux.Dockerfile plus what the regression checks
# call (curl for serve, ffmpeg and LAME for MP3 deliveries).
FROM ubuntu:22.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ make git ca-certificates zlib1g-dev python3 wget curl ffmpeg libmp3lame0 \
    && rm -rf /var/lib/apt/lists/*
RUN wget -qO- https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-$(uname -m).tar.gz \
      | tar xz -C /opt && ln -s /opt/cmake-3.31.6-linux-$(uname -m)/bin/* /usr/local/bin/
