#!/bin/bash
# Container entrypoint: run srcds_linux (32-bit) for CS:GO legacy, LAN only.
# Settings live in csgo/cfg/family.cfg (exec'd by server.cfg and after the
# casual game-mode cfg). Game mode + map group are chosen here on the command line.
set -u
GAME=/opt/csgo/server
PORT="${PORT:-27025}"
[ -x "$GAME/srcds_linux" ] || { echo "[entrypoint] server tree not assembled (assemble.sh)"; exit 1; }
[ -s /run/csgo/rcon_password ] || { echo "[entrypoint] /run/csgo/rcon_password missing"; exit 1; }
RCON_PW="$(tr -d '\r\n' < /run/csgo/rcon_password)"

# The Steam API looks for steamclient.so in ~/.steam/sdk32. bin/steamclient.so is
# the gbe_fork offline emulator (overlay/bin, built from source) and Valve's own
# bin/libsteam_api.so loads it (STEP 2, 2026-09-23: steamclient mode, which the
# csgo_gc launcher ./srcds_linux needs to hook it; before that gbe's libsteam_api.so
# was used). The real steamclient logged on to Steam, the CS:GO GC called build
# 1575 outdated and srcds aborted in a restart loop. Link the whole bin/ dir so
# the emulator finds bin/steam_settings. ./srcds_linux = csgo_gc launcher (local
# Game Coordinator, csgo_gc/); Valve's launcher is srcds_linux.orig.
mkdir -p /root/.steam
rm -rf /root/.steam/sdk32
ln -sfn "$GAME/bin" /root/.steam/sdk32

# srcds silently drops packets whose source is 127.x, and WSL mirrored mode
# delivers a Windows-side client on localhost as 127.0.0.1. So for a client on
# THIS PC: 127.0.0.1:RELAY_PORT -> socat -> <this box's LAN/bridge IP>:PORT (the
# server then sees a private non-loopback source). LAN clients use PORT directly.
RELAY_PORT="${RELAY_PORT:-$((PORT + 1))}"
TARGET="$(hostname -I | tr ' ' '\n' | grep -v '^127\.' | grep -v '^$' | head -1)"
if [ -n "$TARGET" ] && [ "$RELAY_PORT" != 0 ]; then
  # kept alive in a loop: on a container restart socat's bind can fail once
  # ("Interrupted system call") and the relay was then gone until the next restart.
  ( while true; do
      socat -T 60 "UDP4-LISTEN:$RELAY_PORT,bind=127.0.0.1,fork,reuseaddr" "UDP4:$TARGET:$PORT"
      sleep 2
    done ) &
  echo "[entrypoint] local relay 127.0.0.1:$RELAY_PORT -> $TARGET:$PORT"
fi

cd "$GAME"
export LD_LIBRARY_PATH="$GAME:$GAME/bin:${LD_LIBRARY_PATH:-}"
exec ./srcds_linux -game csgo -console -usercon -strictportbind -ip 0.0.0.0 \
  -port "$PORT" +clientport $((PORT + 10)) -nohltv -tickrate 64 \
  -maxplayers_override 12 -nobreakpad -norestart \
  +sv_lan 1 +game_type 0 +game_mode 0 +mapgroup mg_family \
  +rcon_password "$RCON_PW" +map "${START_MAP:-de_dust2}" ${EXTRA_ARGS:-}
