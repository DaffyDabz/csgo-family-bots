#!/bin/bash
# Copy this repo folder (servers/csgo) to /opt/csgo in WSL. Run as root in WSL:
#   bash <your-path>/csgo-family-bots/deploy.sh [--assemble]
# Never touches /opt/csgo/server's game files or rcon_password; the overlay cfgs
# are re-applied by `csgo-ctl.sh assemble` (or pass --assemble here).
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
mkdir -p /opt/csgo
for f in Dockerfile docker-compose.yml entrypoint.sh csgo-ctl.sh assemble.sh rcon.py; do
  install -m 755 "$SRC/$f" "/opt/csgo/$f"
done
chmod 644 /opt/csgo/Dockerfile /opt/csgo/docker-compose.yml
rm -rf /opt/csgo/overlay && cp -r "$SRC/overlay" /opt/csgo/overlay
# strip CR in case a Windows checkout converted endings
sed -i 's/\r$//' /opt/csgo/*.sh /opt/csgo/rcon.py
# STEP 9: the bot brain's map-point generator (run by assemble.sh)
install -m 755 "$SRC/bots/make_mapinfo.py" /opt/csgo/make_mapinfo.py && sed -i 's/\r$//' /opt/csgo/make_mapinfo.py
# text files only: overlay/ also carries binaries (steamclient.so, srcds_linux, csgo_gc.so)
find /opt/csgo/overlay -type f \( -name '*.cfg' -o -name '*.txt' -o -name '*.ini' \) -exec sed -i 's/\r$//' {} +
echo "[deploy] /opt/csgo updated"
if [ "${1:-}" = "--assemble" ]; then bash /opt/csgo/csgo-ctl.sh assemble; fi
