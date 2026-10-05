#!/bin/bash
# Build the family_party server plugin (32-bit Linux .so, static C++ runtime) in the throwaway csgo_gc builder image
# (family/csgo-gc-builder: any Ubuntu image with g++-multilib works, see README), sources in a tmpfs. Run as root:
#   bash plugin/build.sh
# Output: ../overlay/csgo/addons/family_party.so (deployed by deploy.sh + csgo-ctl.sh assemble).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../overlay/csgo/addons"
mkdir -p "$OUT"
docker run --rm --tmpfs /build:rw,exec,size=256m -v "$HERE":/src:ro -v "$OUT":/out family/csgo-gc-builder:latest bash -c '
  set -e
  cp /src/family_party.cpp /src/family_brain.cpp /src/family_logic.h /src/test_pick.cpp /build/
  g++ -m32 -O2 -std=c++17 -o /build/test_pick /build/test_pick.cpp /build/family_party.cpp /build/family_brain.cpp -lpthread
  /build/test_pick || { echo "UNIT TEST FAILED"; exit 1; }
  g++ -m32 -O2 -std=c++17 -shared -fPIC -fvisibility=hidden -static-libstdc++ -static-libgcc -Wall \
      -o /build/family_party.so /build/family_party.cpp /build/family_brain.cpp -lpthread
  cp /build/family_party.so /out/family_party.so
  file /out/family_party.so
  objdump -T /out/family_party.so | grep -E "CreateInterface|GLIBC_2\.3[0-9]" || true'
