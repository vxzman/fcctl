#!/bin/sh
# Creates a real macvtap when root (or passwordless sudo) is available.
# Without that privilege the script skips and exits 0.
set -eu

BIN=${1:?need fcctl path}
if [ ! -x "$BIN" ]; then
    echo "macvtap.sh: $BIN is not executable" >&2
    exit 1
fi
case "$BIN" in
    /*) ;;
    *) BIN=$(CDPATH= cd -- "$(dirname "$BIN")" && pwd)/$(basename "$BIN") ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    if sudo -n true 2>/dev/null; then
        exec sudo -n "$0" "$BIN"
    fi
    echo "macvtap.sh skipped (need root or passwordless sudo)"
    exit 0
fi

IP=$(command -v ip || true)
if [ -z "$IP" ]; then
    echo "macvtap.sh skipped (no ip)"
    exit 0
fi

suf=$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')
parent=xfcp$suf
tap=xfct$suf
TMP=$(mktemp -d)
cleanup() {
    "$IP" link del "$tap" 2>/dev/null || true
    "$IP" link del "$parent" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT

mkdir -p "$TMP/run"
cat > "$TMP/host.json" << EOF
{
  "network": [{ "type": "macvtap", "master": "$parent", "mode": "bridge" }],
  "runtime_dir": "$TMP/run",
  "config_dir": "$TMP"
}
EOF

write_vm() {
    mac=$1
    cat > "$TMP/vm.json" << EOF
{
  "boot-source": {"kernel_image_path": "/no/such/kernel"},
  "network-interfaces": [{
    "iface_id": "eth0",
    "guest_mac": "$mac",
    "host_dev_name": "$tap"
  }]
}
EOF
}

"$IP" link add "$parent" type bridge
"$IP" link set "$parent" up

write_vm ""
set +e
missing=$("$BIN" --config "$TMP/host.json" --prepare "$TMP/vm.json" 2>&1)
missing_code=$?
set -e
echo "$missing"
test "$missing_code" -ne 0
echo "$missing" | grep -q "requires guest_mac"
test ! -e "/sys/class/net/$tap"

"$IP" link add "$tap" type bridge
write_vm "06:00:AC:10:00:02"
set +e
occupied=$("$BIN" --config "$TMP/host.json" --prepare "$TMP/vm.json" 2>&1)
occupied_code=$?
set -e
echo "$occupied"
test "$occupied_code" -ne 0
echo "$occupied" | grep -q "is not a macvtap device"
# Still the bridge we created, not replaced.
test -d "/sys/class/net/$tap"
idx=$(cat "/sys/class/net/$tap/ifindex")
test ! -e "/sys/class/macvtap/tap$idx"
"$IP" link del "$tap"

write_vm "06:00:AC:10:00:02"
"$BIN" --config "$TMP/host.json" --prepare "$TMP/vm.json"
addr=$(cat "/sys/class/net/$tap/address")
test "$addr" = "06:00:ac:10:00:02"
idx=$(cat "/sys/class/net/$tap/ifindex")
test -e "/sys/class/macvtap/tap$idx"
test -c "/dev/tap$idx"
# Second prepare keeps the device and rewrites the same address.
"$BIN" --config "$TMP/host.json" --prepare "$TMP/vm.json"
test "$(cat "/sys/class/net/$tap/address")" = "06:00:ac:10:00:02"

"$BIN" --config "$TMP/host.json" --clean "$TMP/vm.json"
test ! -e "/sys/class/net/$tap"
test -d "/sys/class/net/$parent"
echo "macvtap.sh ok"
