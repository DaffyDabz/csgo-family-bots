#!/bin/bash
# Build /opt/csgo/server without copying the 32 GB of game content.
#  1. depot 740 (Linux server bins, ~2 GB, $CSGO_SERVER_BIN) -> real copy
#  2. depot 731 (content, identical manifest to the client's) -> symlinks into the
#     client install $CSGO_CLIENT (bind-mounted read-only into the
#     container at the same path), EXCEPT small writable text/config files
#     (csgo/cfg/**, top-level csgo/*.txt|*.cfg|*.ini) which are real copies
#  3. overlay/ (our cfgs, map group) on top
# Idempotent: rerun any time (csgo-ctl.sh assemble). Never writes to the client install.
set -euo pipefail
BIN=${CSGO_SERVER_BIN:-/srv/csgo/server-bin}
CLIENT=${CSGO_CLIENT:-/srv/csgo/game}
MANIFEST=$(ls ${CSGO_MANIFEST_DIR:-/srv/csgo/ds-manifest}/manifest_731_*.txt | head -1)
DEST=/opt/csgo/server
[ -x "$BIN/srcds_linux" ] || { echo "no depot 740 at $BIN"; exit 1; }
[ -f "$CLIENT/csgo/steam.inf" ] || { echo "no client install at $CLIENT"; exit 1; }
mkdir -p "$DEST"

echo "[assemble] depot 740 -> $DEST"
rsync -a --exclude .DepotDownloader "$BIN/" "$DEST/"
chmod +x "$DEST/srcds_linux" "$DEST/srcds_run"
# Valve's bundled 2013 libgcc_s breaks Ubuntu 22.04's libstdc++ ("GCC_7.0.0 not
# found"); the system lib32gcc-s1 replaces it.
rm -f "$DEST/bin/libgcc_s.so.1"

echo "[assemble] depot 731 -> symlinks ($MANIFEST)"
python3 - "$MANIFEST" "$BIN" "$CLIENT" "$DEST" <<'PY'
import os, re, shutil, sys
manifest, bindir, client, dest = sys.argv[1:5]
row = re.compile(r"^\s*(\d+)\s+(\d+)\s+([0-9a-f]{40})\s+(\d+)\s+(.+?)\s*$")
links = copies = skipped = 0
missing = []
def realcopy(p):
    low = p.lower()
    # steam.inf: csgo_gc rewrites its appID (4465480) at start; the client's copy
    # is mounted read-only, so the server keeps its own writable copy.
    if low.startswith("csgo/cfg/") or low == "csgo/steam.inf":
        return True
    return low.count("/") == 1 and low.startswith("csgo/") and low.endswith((".txt", ".cfg", ".ini"))
for line in open(manifest, encoding="utf-8", errors="replace"):
    m = row.match(line)
    if not m:
        continue
    rel = m.group(5).replace("\\", "/")
    src = os.path.join(client, rel)
    if os.path.isdir(src) or int(m.group(4)) & 64:
        continue
    if os.path.lexists(os.path.join(bindir, rel)):
        skipped += 1                      # depot 740 provides this path
        continue
    if not os.path.exists(src):
        missing.append(rel)
        continue
    out = os.path.join(dest, rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if realcopy(rel):
        if os.path.islink(out):
            os.unlink(out)
        if not os.path.exists(out):
            shutil.copy2(src, out)
        copies += 1
    else:
        if os.path.islink(out) and os.readlink(out) == src:
            links += 1
            continue
        if os.path.lexists(out):
            os.unlink(out)
        os.symlink(src, out)
        links += 1
print("[assemble] symlinks %d, real copies %d, provided by 740 %d, missing in client %d"
      % (links, copies, skipped, len(missing)))
for rel in missing[:10]:
    print("  missing:", rel)
PY

echo "[assemble] overlay"
# overlay/bin carries the gbe_fork offline Steam emulator in STEAMCLIENT mode
# (steamclient.so + steam_settings/, app id 4465480); Valve's libsteam_api.so
# stays and loads it from ~/.steam/sdk32 (entrypoint.sh). overlay/srcds_linux is
# the csgo_gc launcher (loads bin/dedicated.so + csgo_gc/csgo_gc.so, the local
# Game Coordinator). Keeps srcds off real Steam and the real GC.
# Valve's originals stay beside them as *.orig (undo: rename them back).
for f in steamclient.so libsteam_api.so; do
  cp -f "$BIN/bin/$f" "$DEST/bin/$f.orig"
done
cp -f "$BIN/srcds_linux" "$DEST/srcds_linux.orig"
# STEP 8 (2026-09-24): an overlay file that replaces a depot-731 symlink (csgo/botprofile.db) must drop the link
# first, or cp writes THROUGH it into the client install. Undo of the bot roster: delete
# overlay/csgo/botprofile.db and re-run assemble (the symlink to Valve's file comes back above).
(cd /opt/csgo/overlay && find . -type f) | while read -r rel; do
  if [ -L "$DEST/$rel" ]; then rm -f "$DEST/$rel"; fi
done
cp -rf /opt/csgo/overlay/. "$DEST/"
chmod +x "$DEST/srcds_linux"
# STEP 9 (2026-09-24): the bot brain's map points (bomb sites, holds, stacks, lurk spots, ...) for every installed map,
# derived from its .nav + .bsp (bots/make_mapinfo.py). A map without them just has stock bots.
if [ -f /opt/csgo/make_mapinfo.py ]; then
  python3 /opt/csgo/make_mapinfo.py "$DEST/csgo" "$DEST/csgo/addons/family_maps" | tail -1
fi
# STEP 2 (2026-09-23): the gbe libsteam_api.so is no longer in overlay/bin; the
# rsync above already put Valve's back. Its steam_api-mode settings are gone too.
echo "[assemble] done: $(du -sh "$DEST" | cut -f1) (symlinks not followed)"
