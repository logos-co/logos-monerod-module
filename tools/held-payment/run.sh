#!/bin/sh
# A payment held while Tor is down stays off clearnet across a restart: see driver.cpp. Builds
# node, peer and driver against flake.lock's pins (logos-monero-nix's monerod-c and monero-c,
# nixpkgs' nlohmann_json) unless MONEROD_C, MONERO_C or JSON_INC say otherwise; CXX defaults to
# c++. Works in ./held-payment and takes about 25 minutes, most of it waiting for monerod's retries.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CXX=${CXX:-c++}

# The rev flake.lock pins for an input, following `follows` paths.
pin() {
  python3 -I -c '
import json, sys
nodes = json.load(open(sys.argv[1]))["nodes"]
def node(path):
    name = "root"
    for step in path:
        ref = nodes[name]["inputs"][step]
        name = node(ref) if isinstance(ref, list) else ref
    return name
print(nodes[node(sys.argv[2:])]["locked"]["rev"])' "$ROOT/flake.lock" "$@"
}
MONERO_NIX=github:logos-co/logos-monero-nix/$(pin logos-monero-nix)
NIXPKGS=github:NixOS/nixpkgs/$(pin logos-module-builder nixpkgs)
MONEROD_C=${MONEROD_C:-$(nix build --no-link --print-out-paths "$MONERO_NIX#monerod-c")}
MONERO_C=${MONERO_C:-$(nix build --no-link --print-out-paths "$MONERO_NIX#monero-c")}
JSON_INC=${JSON_INC:-$(nix build --no-link --print-out-paths "$NIXPKGS#nlohmann_json")/include}

W=$PWD/held-payment
rm -rf "$W"; mkdir -p "$W/node" "$W/peer" "$W/wallets"
"$CXX" -std=c++17 -O1 -I"$ROOT/src" -I"$MONEROD_C/include" -I"$JSON_INC" "$ROOT/src/monerod_settings.cpp" \
  "$HERE/node.cpp" -L"$MONEROD_C/lib" -lmonerod_c -Wl,-rpath,"$MONEROD_C/lib" -lpthread -o "$W/node-bin"
"$CXX" -std=c++17 -O1 -I"$MONEROD_C/include" "$HERE/peer.cpp" \
  -L"$MONEROD_C/lib" -lmonerod_c -Wl,-rpath,"$MONEROD_C/lib" -lpthread -o "$W/peer-bin"
"$CXX" -std=c++17 -O1 -I"$MONERO_C/include" -I"$JSON_INC" "$HERE/driver.cpp" \
  -L"$MONERO_C/lib" -lmonero_wallet2_api_c -Wl,-rpath,"$MONERO_C/lib" -lpthread -o "$W/driver"

free_port() { python3 -I -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }
NODE_RPC=$(free_port); NODE_P2P=$(free_port); PEER_RPC=$(free_port); PEER_P2P=$(free_port); DEAD=$(free_port)

PIDS=""
trap 'for p in $PIDS; do kill "$p" 2> /dev/null || true; done; for p in $PIDS; do wait "$p" 2> /dev/null || true; done' EXIT
trap 'exit 1' INT TERM
"$W/peer-bin" "$W/peer" "$PEER_RPC" "$PEER_P2P" "$NODE_P2P" > "$W/peer.out" 2>&1 &
PIDS="$!"
"$W/node-bin" "$W/node" "$NODE_RPC" "$NODE_P2P" "$PEER_P2P" "$DEAD" > "$W/node.out" 2>&1 &
NODE=$!
PIDS="$PIDS $NODE"
for port in "$PEER_RPC" "$NODE_RPC"; do
  for _ in $(seq 1 120); do
    python3 -I -c 'import sys,urllib.request;urllib.request.urlopen("http://127.0.0.1:"+sys.argv[1]+"/get_height",timeout=2)' "$port" 2> /dev/null && break
    sleep 0.5
  done
done
grep '^argv ' "$W/node.out"
"$W/driver" "$W/wallets" "$NODE_RPC" "$PEER_RPC" "$NODE" "$W/node/node.log"
