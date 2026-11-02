#pragma once

#include <string>

// Host-side devices for Firecracker NICs. Firecracker itself opens a TAP
// (`network-interfaces[].host_dev_name` via TUNSETIFF). fcctl can also create
// macvtap/macvlan on a parent; those are host construction, not a guest backend.

bool iface_exists(const std::string& name);

// Persistent tap: IFF_TAP | IFF_NO_PI, then TUNSETPERSIST. Firecracker turns
// on IFF_VNET_HDR itself when it opens the device.
void create_tap(const std::string& name);
void create_macvlan(const std::string& name, const std::string& master, const std::string& mode);
void create_macvtap(const std::string& name, const std::string& master, const std::string& mode);
void set_master(const std::string& name, const std::string& bridge);
void set_up(const std::string& name);

// Missing devices are not an error.
void delete_link(const std::string& name);
