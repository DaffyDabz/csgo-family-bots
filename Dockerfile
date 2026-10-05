# CS:GO (legacy 1.38.8.1) dedicated server, LAN only. The image holds only the
# 32-bit runtime libs; the server tree is /opt/csgo/server on the WSL host
# (assembled by assemble.sh: depot 740 copied + depot 731 symlinked to the
# client install), bind-mounted at /opt/csgo/server.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates tini socat hostname libc6-i386 lib32gcc-s1 lib32stdc++6 lib32z1 \
      libc6:i386 libstdc++6:i386 libgcc-s1:i386 zlib1g:i386 libtinfo6:i386 libncurses6:i386 \
 && rm -rf /var/lib/apt/lists/*
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
ENTRYPOINT ["/usr/bin/tini", "--", "/entrypoint.sh"]
