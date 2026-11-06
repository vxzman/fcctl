#pragma once

#include <string>

// Host-side devices for Firecracker NICs.
// tap/bridge: Firecracker opens host_dev_name through /dev/net/tun + TUNSETIFF.
// macvtap: Firecracker opens the existing /dev/tap<ifindex> and does not create
// or delete it. guest_mac must be the macvtap MAC. macvlan is not a TAP.

bool iface_exists(const std::string& name);
bool is_macvtap(const std::string& name);

// Persistent tap: IFF_TAP | IFF_NO_PI, then TUNSETPERSIST. Firecracker turns
// on IFF_VNET_HDR itself when it opens the device.
void create_tap(const std::string& name);
void create_macvlan(const std::string& name, const std::string& master, const std::string& mode);
void create_macvtap(const std::string& name, const std::string& master, const std::string& mode);
void set_master(const std::string& name, const std::string& bridge);
void set_up(const std::string& name);

// Copy guest_mac onto an existing macvtap, then make /dev/tap<ifindex> openable
// by this process. Firecracker is exec'd as the same user.
void prepare_macvtap(const std::string& name, const std::string& mac);

// Missing devices are not an error.
void delete_link(const std::string& name);
