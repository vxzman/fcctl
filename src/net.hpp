#pragma once

#include <string>

// Plain tap devices on a bridge. Firecracker opens the tap by name and does
// not support macvtap. Creation matches `ip tuntap add <name> mode tap`.

bool iface_exists(const std::string& name);

// Persistent tap: IFF_TAP | IFF_NO_PI, then TUNSETPERSIST. Firecracker turns
// on IFF_VNET_HDR itself when it opens the device.
void create_tap(const std::string& name);
void set_master(const std::string& name, const std::string& bridge);
void set_up(const std::string& name);

// Missing devices are not an error.
void delete_link(const std::string& name);
