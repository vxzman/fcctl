#!/bin/sh
# CLI checks that do not need root or a live Firecracker.
set -eu
BIN=${1:?need fcctl path}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/good" "$TMP/bad" "$TMP/out"
cp "$ROOT/examples/debian.json" "$TMP/good/debian.json"
cp "$ROOT/examples/debian.json" "$TMP/bad/debian.json"
sed \
  -e 's/debian.raw/debian.raw/' \
  -e 's/fc-debian/fc-debian/' \
  "$ROOT/examples/debian.json" > "$TMP/bad/clone.json"
# clone.json still has debian's disk, MAC, and tap. Rename is not enough;
# the file name makes it a second VM that collides on all three.

set +e
good=$(FCCTL_CONFIG_DIR="$TMP/good" FCCTL_BRIDGE=brg27 "$BIN" check 2>&1)
good_code=$?
bad=$(FCCTL_CONFIG_DIR="$TMP/bad" FCCTL_BRIDGE=brg27 "$BIN" check 2>&1)
bad_code=$?
set -e

echo "$good"
echo "$bad"
test "$good_code" -eq 0
test "$bad_code" -ne 0
echo "$bad" | grep -q "disk conflict:"
echo "$bad" | grep -q "MAC conflict:"
echo "$bad" | grep -q "tap conflict:"

unit=$(FCCTL_BIN=/usr/local/bin/fcctl FCCTL_CONFIG_DIR="$TMP/good" \
  "$BIN" generate "$TMP/good/debian.json")
echo "$unit" | grep -q "SyslogIdentifier=debian.fc"
echo "$unit" | grep -q 'ExecStart="/usr/local/bin/fcctl" run "'
echo "$unit" | grep -q 'Requires=dev-ubuntu\\x2dvg-debian.raw.device'
echo "$unit" | grep -q "ExecStopPost="

cat > "$TMP/host.json" << EOF
{
  "bridge": "brg27",
  "config_dir": "$TMP/good",
  "unit_dir": "$TMP/out",
  "fcctl_bin": "/usr/local/bin/fcctl"
}
EOF
FCCTL_CONFIG="$TMP/host.json" "$BIN" generate --all --write
test -f "$TMP/out/debian.fc.service"
grep -q "debian.fc" "$TMP/out/debian.fc.service"
echo "check.sh ok"
