#include "json.hpp"
#include "net.hpp"

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

// systemd Restart=on-failure must not spin when the API socket is already taken.
constexpr int kExitAlreadyRunning = 69;

struct ExitCode : std::runtime_error {
    int code;
    ExitCode(int exit_code, const std::string& message)
        : std::runtime_error(message), code(exit_code) {}
};

namespace {

constexpr const char* kVersion = "0.2.0";

struct Disk {
    std::string path;
    bool readonly = false;
};

struct Nic {
    std::string iface_id;
    std::string mac;
    std::string host_dev;
};

struct Vm {
    std::string name;
    std::string file;
    std::string kernel;
    std::string initrd;
    std::vector<Disk> disks;
    std::vector<Nic> nics;
};

struct NetSpec {
    std::string name;
    std::string type;
    std::string master;
    std::string mode;
};

struct Host {
    std::string config_path = "/etc/fcctl/config.json";
    std::vector<NetSpec> network;
    std::string binary;
    std::string config_dir = "/etc/fcctl";
    std::string runtime_dir = "/run/firecracker";
    std::string fcctl_bin = "/usr/local/bin/fcctl";
    std::string unit_dir = "/etc/systemd/system";
    int stop_timeout_secs = 45;
    int timeout_stop_sec = 90;
};

struct Report {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::set<std::string> bad_vms;
};

void info(const std::string& message) { std::cout << "fcctl: " << message << "\n"; }

void warn(const std::string& message) {
    std::cerr << "fcctl: warning: " << message << "\n";
}

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

[[noreturn]] void fail_code(int code, const std::string& message) {
    throw ExitCode(code, message);
}

bool is_dir(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        fail("cannot read '" + path + "'");
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string real_path(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) == nullptr) {
        fail("cannot resolve '" + path + "': " + std::strerror(errno));
    }
    return buf;
}

std::string real_path_or_literal(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) == nullptr) {
        return path;
    }
    return buf;
}

std::string stem_of(const std::string& path) {
    const auto slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    constexpr const char* kSuffix = ".json";
    if (base.size() > 5 && base.compare(base.size() - 5, 5, kSuffix) == 0) {
        base.resize(base.size() - 5);
    }
    return base;
}

bool valid_vm_name(const std::string& name) {
    if (name.empty() || name.size() > 63 || name == "." || name == "..") {
        return false;
    }
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool valid_ifname(const std::string& name) {
    if (name.empty() || name.size() > 15 || name == "." || name == "..") {
        return false;
    }
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool valid_mac(const std::string& mac) {
    if (mac.size() != 17) {
        return false;
    }
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i % 3 == 2) {
            if (mac[i] != ':') {
                return false;
            }
            continue;
        }
        const char c = mac[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

std::string lower_mac(std::string mac) {
    for (char& c : mac) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return mac;
}

std::string env_or(const char* key, const std::string& fallback) {
    if (const char* value = std::getenv(key)) {
        if (*value != '\0') {
            return value;
        }
    }
    return fallback;
}

bool valid_net_type(const std::string& type) {
    return type == "tap" || type == "bridge" || type == "macvtap" || type == "macvlan";
}

bool valid_macvlan_mode(const std::string& mode) {
    return mode == "bridge" || mode == "vepa" || mode == "private" || mode == "passthru";
}

bool needs_master(const std::string& type) { return type != "tap"; }

void validate_net_spec(const NetSpec& spec, const std::string& path, std::size_t index) {
    const std::string where = path + ": network[" + std::to_string(index) + "]";
    if (!valid_net_type(spec.type)) {
        fail(where + ": type must be tap, bridge, macvtap, or macvlan");
    }
    if (!spec.name.empty() && !valid_ifname(spec.name)) {
        fail(where + ": name '" + spec.name + "' is not a usable interface name");
    }
    if (needs_master(spec.type)) {
        if (spec.master.empty() || spec.master.find('/') != std::string::npos ||
            !valid_ifname(spec.master)) {
            fail(where + ": master must be an interface name");
        }
    } else if (!spec.master.empty()) {
        fail(where + ": tap does not take master");
    }
    if (spec.type == "macvlan" || spec.type == "macvtap") {
        if (!valid_macvlan_mode(spec.mode)) {
            fail(where + ": mode must be bridge, vepa, private, or passthru");
        }
    } else if (!spec.mode.empty()) {
        fail(where + ": mode is only valid for macvlan and macvtap");
    }
}

NetSpec parse_net_spec(const Json& item, const std::string& path, std::size_t index) {
    const std::string where = path + ": network[" + std::to_string(index) + "]";
    if (item.type != Json::Type::Object) {
        fail(where + " is not an object");
    }
    for (const auto& field : item.o) {
        const std::string& key = field.first;
        if (key != "type" && key != "master" && key != "name" && key != "mode") {
            fail(where + ": unknown key \"" + key + "\"");
        }
        if (field.second.type != Json::Type::String) {
            fail(where + ": " + key + " must be a string");
        }
    }
    NetSpec spec;
    spec.type = item.str("type");
    spec.master = item.str("master");
    spec.name = item.str("name");
    spec.mode = item.str("mode");
    if (spec.type.empty()) {
        fail(where + " is missing type");
    }
    if ((spec.type == "macvlan" || spec.type == "macvtap") && spec.mode.empty()) {
        spec.mode = "bridge";
    }
    validate_net_spec(spec, path, index);
    return spec;
}

void check_network_unique(const std::vector<NetSpec>& network, const std::string& path) {
    bool have_default = false;
    std::set<std::string> names;
    for (const NetSpec& spec : network) {
        if (spec.name.empty()) {
            if (have_default) {
                fail(path + ": only one default network entry is allowed");
            }
            have_default = true;
        } else if (!names.insert(spec.name).second) {
            fail(path + ": duplicate network name '" + spec.name + "'");
        }
    }
}

void apply_host_json(Host& host, const Json& root, const std::string& path) {
    if (root.type != Json::Type::Object) {
        fail(path + " must be a JSON object");
    }
    for (const auto& item : root.o) {
        const std::string& key = item.first;
        if (key == "bridge" || key == "binary" || key == "config_dir" || key == "runtime_dir" ||
            key == "fcctl_bin" || key == "unit_dir") {
            if (item.second.type != Json::Type::String) {
                fail(path + ": " + key + " must be a string");
            }
        } else if (key == "stop_timeout_secs" || key == "timeout_stop_sec") {
            if (item.second.type != Json::Type::Number || !item.second.is_int || item.second.i <= 0) {
                fail(path + ": " + key + " must be a positive integer");
            }
        } else if (key == "network") {
            if (item.second.type != Json::Type::Array) {
                fail(path + ": network must be an array");
            }
        } else {
            fail(path + ": unknown key \"" + key + "\"");
        }
    }
    std::string bridge;
    const bool had_bridge = root.find("bridge") != nullptr;
    if (had_bridge) {
        bridge = root.find("bridge")->s;
        if (bridge.empty() || bridge.find('/') != std::string::npos || !valid_ifname(bridge)) {
            fail(path + ": bridge must be an interface name");
        }
    }
    const bool had_network = root.find("network") != nullptr;
    if (had_network) {
        const Json* list = root.find("network");
        for (std::size_t i = 0; i < list->a.size(); ++i) {
            host.network.push_back(parse_net_spec(list->a[i], path, i));
        }
        check_network_unique(host.network, path);
    }
    if (had_network && had_bridge) {
        fail(path + ": set network or bridge, not both");
    }
    if (!had_network) {
        NetSpec spec;
        spec.type = "bridge";
        spec.master = had_bridge ? bridge : "brg27";
        host.network.push_back(std::move(spec));
    }
    if (const Json* value = root.find("binary")) {
        host.binary = value->s;
    }
    if (const Json* value = root.find("config_dir")) {
        host.config_dir = value->s;
    }
    if (const Json* value = root.find("runtime_dir")) {
        host.runtime_dir = value->s;
    }
    if (const Json* value = root.find("fcctl_bin")) {
        host.fcctl_bin = value->s;
    }
    if (const Json* value = root.find("unit_dir")) {
        host.unit_dir = value->s;
    }
    if (const Json* value = root.find("stop_timeout_secs")) {
        host.stop_timeout_secs = static_cast<int>(value->i);
    }
    if (const Json* value = root.find("timeout_stop_sec")) {
        host.timeout_stop_sec = static_cast<int>(value->i);
    }
}

void apply_bridge_env(Host& host) {
    const char* value = std::getenv("FCCTL_BRIDGE");
    if (value == nullptr || *value == '\0') {
        return;
    }
    NetSpec* unnamed = nullptr;
    for (NetSpec& spec : host.network) {
        if (spec.name.empty()) {
            unnamed = &spec;
            break;
        }
    }
    if (unnamed == nullptr) {
        fail("FCCTL_BRIDGE requires a default network entry");
    }
    if (unnamed->type != "bridge") {
        fail("FCCTL_BRIDGE can only override a default bridge entry");
    }
    unnamed->master = value;
}

Host load_host(const std::string& cli_config) {
    Host host;
    bool explicit_config = false;
    if (!cli_config.empty()) {
        host.config_path = cli_config;
        explicit_config = true;
    } else if (const char* env = std::getenv("FCCTL_CONFIG")) {
        host.config_path = env;
        explicit_config = true;
    }
    if (is_file(host.config_path)) {
        apply_host_json(host, parse_json(read_file(host.config_path)), host.config_path);
    } else if (explicit_config) {
        fail("global config '" + host.config_path + "' does not exist");
    } else {
        NetSpec spec;
        spec.type = "bridge";
        spec.master = "brg27";
        host.network.push_back(std::move(spec));
    }
    host.config_dir = env_or("FCCTL_CONFIG_DIR", host.config_dir);
    apply_bridge_env(host);
    host.runtime_dir = env_or("FCCTL_RUNTIME", host.runtime_dir);
    host.binary = env_or("FCCTL_FIRECRACKER", host.binary);
    return host;
}

NetSpec spec_for_nic(const Host& host, const std::string& ifname) {
    if (host.network.empty()) {
        NetSpec tap;
        tap.type = "tap";
        return tap;
    }
    const NetSpec* named = nullptr;
    const NetSpec* unnamed = nullptr;
    for (const NetSpec& spec : host.network) {
        if (spec.name == ifname) {
            named = &spec;
        } else if (spec.name.empty()) {
            unnamed = &spec;
        }
    }
    if (named != nullptr) {
        return *named;
    }
    if (unnamed != nullptr) {
        return *unnamed;
    }
    fail("no network entry matches host_dev_name '" + ifname + "'");
}

bool looks_like_vm(const Json& root) {
    return root.type == Json::Type::Object &&
           (root.find("boot-source") || root.find("drives") || root.find("machine-config") ||
            root.find("network-interfaces"));
}

Vm parse_vm(const std::string& path) {
    Json root;
    try {
        root = parse_json(read_file(path));
    } catch (const std::exception& err) {
        fail(path + ": " + err.what());
    }
    if (!looks_like_vm(root)) {
        fail(path + " is not a Firecracker config");
    }
    Vm vm;
    vm.file = path;
    vm.name = stem_of(path);
    if (!valid_vm_name(vm.name)) {
        fail("VM name '" + vm.name + "' in '" + path +
             "' must be 1..=63 characters from [A-Za-z0-9_.-]");
    }
    try {
        if (const Json* boot = root.find("boot-source")) {
            if (boot->type != Json::Type::Object) {
                fail("boot-source is not an object");
            }
            vm.kernel = boot->str("kernel_image_path");
            vm.initrd = boot->str("initrd_path");
        }
        if (const Json* drives = root.find("drives")) {
            if (drives->type != Json::Type::Array) {
                fail("drives is not an array");
            }
            for (const Json& item : drives->a) {
                if (item.type != Json::Type::Object) {
                    fail("drive entry is not an object");
                }
                Disk disk;
                disk.path = item.str("path_on_host");
                disk.readonly = item.flag("is_read_only", false);
                if (disk.path.empty()) {
                    fail("drive is missing path_on_host");
                }
                vm.disks.push_back(std::move(disk));
            }
        }
        if (const Json* nics = root.find("network-interfaces")) {
            if (nics->type != Json::Type::Array) {
                fail("network-interfaces is not an array");
            }
            for (const Json& item : nics->a) {
                if (item.type != Json::Type::Object) {
                    fail("network entry is not an object");
                }
                Nic nic;
                nic.iface_id = item.str("iface_id");
                nic.mac = item.str("guest_mac");
                nic.host_dev = item.str("host_dev_name");
                if (nic.host_dev.empty()) {
                    fail("network interface is missing host_dev_name");
                }
                if (!valid_ifname(nic.host_dev)) {
                    fail("host_dev_name '" + nic.host_dev + "' is not a usable interface name");
                }
                if (!nic.mac.empty() && !valid_mac(nic.mac)) {
                    fail("guest_mac '" + nic.mac + "' must look like 0a:fc:00:00:02:00");
                }
                vm.nics.push_back(std::move(nic));
            }
        }
    } catch (const std::runtime_error& err) {
        fail(path + ": " + err.what());
    }
    return vm;
}

std::vector<std::string> list_json(const std::string& dir) {
    std::vector<std::string> paths;
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
        fail("cannot read '" + dir + "': " + std::strerror(errno));
    }
    while (dirent* entry = ::readdir(handle)) {
        const std::string name = entry->d_name;
        if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".json") != 0) {
            continue;
        }
        if (name == "config.json") {
            continue;
        }
        const std::string path = dir + "/" + name;
        if (is_file(path)) {
            paths.push_back(path);
        }
    }
    ::closedir(handle);
    return paths;
}

void load_dir(const std::string& dir, std::vector<Vm>& vms, Report& report) {
    if (!is_dir(dir)) {
        return;
    }
    auto paths = list_json(dir);
    for (const std::string& path : paths) {
        Json root;
        try {
            root = parse_json(read_file(path));
        } catch (const std::exception& err) {
            report.errors.push_back(path + ": " + err.what());
            continue;
        }
        if (!looks_like_vm(root)) {
            report.warnings.push_back("skipping '" + path + "', not a Firecracker config");
            continue;
        }
        try {
            vms.push_back(parse_vm(path));
        } catch (const std::exception& err) {
            report.errors.push_back(err.what());
            report.bad_vms.insert(stem_of(path));
        }
    }
}

Report check_vms(const std::vector<Vm>& vms) {
    Report report;
    std::set<std::string> names;
    for (const Vm& vm : vms) {
        if (!names.insert(vm.name).second) {
            report.errors.push_back("duplicate VM name '" + vm.name + "'");
            report.bad_vms.insert(vm.name);
        }
    }

    struct DiskUse {
        std::string vm;
        bool readonly = false;
    };
    std::vector<std::pair<std::string, std::vector<DiskUse>>> disks;
    auto disk_slot = [&](const std::string& path) -> std::vector<DiskUse>& {
        for (auto& item : disks) {
            if (item.first == path) {
                return item.second;
            }
        }
        disks.push_back({path, {}});
        return disks.back().second;
    };

    struct MacUse {
        std::string vm;
        std::string tap;
    };
    std::vector<std::pair<std::string, std::vector<MacUse>>> macs;
    auto mac_slot = [&](const std::string& mac) -> std::vector<MacUse>& {
        for (auto& item : macs) {
            if (item.first == mac) {
                return item.second;
            }
        }
        macs.push_back({mac, {}});
        return macs.back().second;
    };

    std::vector<std::pair<std::string, std::vector<std::string>>> taps;

    for (const Vm& vm : vms) {
        if (!vm.kernel.empty() && !exists(vm.kernel)) {
            report.warnings.push_back("VM '" + vm.name + "' kernel '" + vm.kernel +
                                      "' does not exist yet");
        }
        if (!vm.initrd.empty() && !exists(vm.initrd)) {
            report.warnings.push_back("VM '" + vm.name + "' initrd '" + vm.initrd +
                                      "' does not exist yet");
        }
        for (const Disk& disk : vm.disks) {
            if (!exists(disk.path)) {
                report.warnings.push_back("VM '" + vm.name + "' disk '" + disk.path +
                                          "' does not exist yet");
            }
            disk_slot(real_path_or_literal(disk.path)).push_back({vm.name, disk.readonly});
        }
        for (const Nic& nic : vm.nics) {
            if (nic.mac.empty()) {
                report.warnings.push_back("VM '" + vm.name + "' interface '" + nic.host_dev +
                                          "' has no guest_mac");
            } else {
                mac_slot(lower_mac(nic.mac)).push_back({vm.name, nic.host_dev});
            }
            bool found = false;
            for (auto& item : taps) {
                if (item.first == nic.host_dev) {
                    item.second.push_back(vm.name);
                    found = true;
                    break;
                }
            }
            if (!found) {
                taps.push_back({nic.host_dev, {vm.name}});
            }
        }
    }

    for (const auto& item : disks) {
        if (item.second.size() < 2) {
            continue;
        }
        bool writable = false;
        std::string who;
        for (const DiskUse& use : item.second) {
            if (!use.readonly) {
                writable = true;
            }
            if (!who.empty()) {
                who += ", ";
            }
            who += use.vm + (use.readonly ? "(ro)" : "(rw)");
        }
        if (writable) {
            report.errors.push_back("disk conflict: '" + item.first + "' used by " + who);
            for (const DiskUse& use : item.second) {
                report.bad_vms.insert(use.vm);
            }
        } else {
            report.warnings.push_back("disk '" + item.first + "' shared read-only by " + who);
        }
    }
    for (const auto& item : macs) {
        if (item.second.size() < 2) {
            continue;
        }
        std::string who;
        for (const MacUse& use : item.second) {
            if (!who.empty()) {
                who += ", ";
            }
            who += use.vm + "(" + use.tap + ")";
            report.bad_vms.insert(use.vm);
        }
        report.errors.push_back("MAC conflict: " + item.first + " used by " + who);
    }
    for (const auto& item : taps) {
        if (item.second.size() < 2) {
            continue;
        }
        std::string who;
        for (const std::string& vm : item.second) {
            if (!who.empty()) {
                who += ", ";
            }
            who += vm;
            report.bad_vms.insert(vm);
        }
        report.errors.push_back("tap conflict: '" + item.first + "' used by " + who);
    }
    return report;
}

Report scan(const Host& host, const std::vector<std::string>& extra) {
    std::vector<Vm> vms;
    Report load_errors;
    load_dir(host.config_dir, vms, load_errors);
    std::set<std::string> seen;
    for (const Vm& vm : vms) {
        seen.insert(real_path_or_literal(vm.file));
    }
    for (const std::string& path : extra) {
        if (!is_file(path)) {
            load_errors.errors.push_back("cannot find VM config '" + path + "'");
            continue;
        }
        const std::string canon = real_path_or_literal(path);
        if (seen.count(canon) != 0) {
            continue;
        }
        try {
            vms.push_back(parse_vm(path));
            seen.insert(canon);
        } catch (const std::exception& err) {
            load_errors.errors.push_back(err.what());
        }
    }
    Report report = check_vms(vms);
    report.errors.insert(report.errors.begin(), load_errors.errors.begin(), load_errors.errors.end());
    report.warnings.insert(report.warnings.begin(), load_errors.warnings.begin(),
                           load_errors.warnings.end());
    report.bad_vms.insert(load_errors.bad_vms.begin(), load_errors.bad_vms.end());
    std::set<std::string> masters;
    for (const NetSpec& spec : host.network) {
        if (!spec.master.empty() && masters.insert(spec.master).second &&
            !iface_exists(spec.master)) {
            report.warnings.push_back("master '" + spec.master + "' does not exist yet");
        }
    }
    return report;
}

void print_report(const Report& report) {
    for (const std::string& message : report.warnings) {
        warn(message);
    }
    for (const std::string& message : report.errors) {
        std::cerr << "fcctl: " << message << "\n";
    }
}

// systemd-escape --path
std::string systemd_path_escape(const std::string& path) {
    std::size_t start = 0;
    while (start < path.size() && path[start] == '/') {
        ++start;
    }
    std::string out;
    for (std::size_t i = start; i < path.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        if (c == '/') {
            out.push_back('-');
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == ':' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            constexpr char kHex[] = "0123456789abcdef";
            out += "\\x";
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

std::string unit_binary(const Host& host) {
    if (const char* env = std::getenv("FCCTL_BIN")) {
        if (*env != '\0') {
            return env;
        }
    }
    if (::access(host.fcctl_bin.c_str(), X_OK) == 0) {
        return host.fcctl_bin;
    }
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
    return host.fcctl_bin;
}

int unit_stop_timeout(const Host& host) {
    const int floor = host.stop_timeout_secs + 15;
    return host.timeout_stop_sec > floor ? host.timeout_stop_sec : floor;
}

std::string render_unit(const Host& host, const Vm& vm) {
    std::vector<std::string> after = {"network-online.target"};
    std::vector<std::string> devices;
    std::vector<std::string> mounts;
    auto add_mount = [&](const std::string& path) {
        if (path.empty() || path[0] != '/') {
            return;
        }
        if (path.compare(0, 5, "/dev/") == 0) {
            devices.push_back(systemd_path_escape(path) + ".device");
        } else {
            mounts.push_back(path);
        }
    };
    add_mount(vm.kernel);
    add_mount(vm.initrd);
    for (const Disk& disk : vm.disks) {
        add_mount(disk.path);
    }
    for (const std::string& device : devices) {
        after.push_back(device);
    }
    auto join = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& item : items) {
            if (!out.empty()) {
                out.push_back(' ');
            }
            out += item;
        }
        return out;
    };
    const std::string bin = unit_binary(host);
    const std::string src = vm.file;
    std::ostringstream unit;
    unit << "# Automatically generated by fcctl for " << vm.name << "\n";
    unit << "# Source: " << src << "\n\n";
    unit << "[Unit]\n";
    unit << "Description=Firecracker VM " << vm.name << "\n";
    unit << "SourcePath=" << src << "\n";
    unit << "After=" << join(after) << "\n";
    unit << "Wants=network-online.target\n";
    if (!devices.empty()) {
        unit << "Requires=" << join(devices) << "\n";
    }
    if (!mounts.empty()) {
        unit << "RequiresMountsFor=" << join(mounts) << "\n";
    }
    unit << "\n[Service]\n";
    unit << "Type=simple\n";
    unit << "SyslogIdentifier=" << vm.name << ".fc\n";
    unit << "ExecStart=\"" << bin << "\" --config \"" << host.config_path << "\" --run \"" << src
         << "\"\n";
    unit << "ExecStop=\"" << bin << "\" --config \"" << host.config_path << "\" --stop \"" << src
         << "\"\n";
    unit << "ExecStopPost=-\"" << bin << "\" --config \"" << host.config_path << "\" --clean \""
         << src << "\"\n";
    unit << "TimeoutStopSec=" << unit_stop_timeout(host) << "\n";
    unit << "Restart=on-failure\n";
    unit << "RestartPreventExitStatus=" << kExitAlreadyRunning << "\n";
    unit << "RestartSec=2\n";
    unit << "LimitNOFILE=65536\n";
    unit << "StandardInput=null\n";
    unit << "StandardOutput=journal\n";
    unit << "StandardError=journal\n";
    unit << "\n[Install]\n";
    unit << "WantedBy=multi-user.target\n";
    return unit.str();
}

std::string service_name(const std::string& vm) { return vm + ".fc.service"; }

std::vector<int> version_nums(const std::string& name) {
    std::vector<int> nums;
    const auto at = name.find("-v");
    if (at == std::string::npos) {
        return nums;
    }
    std::string token;
    for (std::size_t i = at + 2; i < name.size(); ++i) {
        const char c = name[i];
        if (c >= '0' && c <= '9') {
            token.push_back(c);
            continue;
        }
        if (!token.empty()) {
            nums.push_back(std::atoi(token.c_str()));
            token.clear();
        }
        if (c != '.') {
            break;
        }
    }
    if (!token.empty()) {
        nums.push_back(std::atoi(token.c_str()));
    }
    return nums;
}

bool newer_bin(const std::string& candidate, const std::string& current) {
    const std::vector<int> a = version_nums(stem_of(candidate));
    const std::vector<int> b = version_nums(stem_of(current));
    const std::size_t n = a.size() > b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv) {
            return av > bv;
        }
    }
    return candidate > current;
}

void consider_bin(std::string& best, const std::string& path) {
    const std::string base = stem_of(path);
    if (base.find("firecracker-v") == std::string::npos || base.find(".debug") != std::string::npos) {
        return;
    }
    if (::access(path.c_str(), X_OK) != 0) {
        return;
    }
    if (best.empty() || newer_bin(path, best)) {
        best = path;
    }
}

std::string find_firecracker(const Host& host) {
    if (!host.binary.empty()) {
        if (::access(host.binary.c_str(), X_OK) != 0) {
            fail("firecracker binary '" + host.binary + "' is not executable");
        }
        return host.binary;
    }
    std::string best;
    DIR* root = ::opendir("/opt/firecracker");
    if (root != nullptr) {
        while (dirent* entry = ::readdir(root)) {
            const std::string name = entry->d_name;
            if (name.rfind("release-", 0) != 0) {
                continue;
            }
            const std::string dir = "/opt/firecracker/" + name;
            DIR* sub = ::opendir(dir.c_str());
            if (sub == nullptr) {
                continue;
            }
            while (dirent* file = ::readdir(sub)) {
                consider_bin(best, dir + "/" + file->d_name);
            }
            ::closedir(sub);
        }
        ::closedir(root);
    }
    if (!best.empty()) {
        return best;
    }
    if (::access("/usr/local/bin/firecracker", X_OK) == 0) {
        return "/usr/local/bin/firecracker";
    }
    if (const char* path = std::getenv("PATH")) {
        std::stringstream ss(path);
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            if (dir.empty()) {
                continue;
            }
            const std::string full = dir + "/firecracker";
            if (::access(full.c_str(), X_OK) == 0) {
                return full;
            }
        }
    }
    fail("cannot find a Firecracker binary under /opt/firecracker or PATH");
}

std::string api_socket(const Host& host, const Vm& vm) {
    return host.runtime_dir + "/" + vm.name + ".sock";
}

int connect_unix(const std::string& path) {
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        fail("API socket path is too long: " + path);
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool api_alive(const std::string& path) {
    const int fd = connect_unix(path);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}

bool write_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string read_some(int fd) {
    timeval timeout{};
    timeout.tv_sec = 2;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    std::string out;
    char buf[1024];
    while (out.size() < 8192) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

int http_status(const std::string& response) {
    if (response.compare(0, 5, "HTTP/") != 0) {
        return 0;
    }
    const auto space = response.find(' ');
    if (space == std::string::npos) {
        return 0;
    }
    return std::atoi(response.c_str() + space + 1);
}

void send_ctrl_alt_del(const std::string& path) {
    const int fd = connect_unix(path);
    if (fd < 0) {
        return;
    }
    const std::string body = "{\"action_type\":\"SendCtrlAltDel\"}";
    const std::string req = "PUT /actions HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
                            "application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (!write_all(fd, req)) {
        ::close(fd);
        fail("failed to write to the Firecracker API socket");
    }
    const std::string response = read_some(fd);
    ::close(fd);
    const int status = http_status(response);
    if (status < 200 || status >= 300) {
        warn("SendCtrlAltDel returned HTTP " + std::to_string(status));
    } else {
        info("sent SendCtrlAltDel");
    }
}

void wait_until_down(const std::string& path, int timeout_secs) {
    const auto start = std::time(nullptr);
    while (std::time(nullptr) - start < timeout_secs) {
        if (!api_alive(path)) {
            return;
        }
        timespec pause{};
        pause.tv_nsec = 200000000;
        ::nanosleep(&pause, nullptr);
    }
    if (api_alive(path)) {
        fail("VM is still running after " + std::to_string(timeout_secs) + "s");
    }
}

std::vector<std::string> prepare_nics(const Host& host, const Vm& vm) {
    std::vector<std::string> created;
    try {
        for (const Nic& nic : vm.nics) {
            const NetSpec spec = spec_for_nic(host, nic.host_dev);
            const bool existed = iface_exists(nic.host_dev);
            if (!existed) {
                if (spec.type == "macvlan") {
                    create_macvlan(nic.host_dev, spec.master, spec.mode);
                } else if (spec.type == "macvtap") {
                    create_macvtap(nic.host_dev, spec.master, spec.mode);
                } else {
                    create_tap(nic.host_dev);
                }
                created.push_back(nic.host_dev);
            }
            if (spec.type == "bridge") {
                set_master(nic.host_dev, spec.master);
            }
            set_up(nic.host_dev);
            std::string line = "prepared " + nic.host_dev + " type=" + spec.type;
            if (!spec.master.empty()) {
                line += " master=" + spec.master;
            }
            if (!spec.mode.empty()) {
                line += " mode=" + spec.mode;
            }
            info(line);
        }
    } catch (...) {
        for (const std::string& name : created) {
            try {
                delete_link(name);
            } catch (const std::exception& err) {
                warn(err.what());
            }
        }
        throw;
    }
    return created;
}

void remove_runtime(const Host& host, const Vm& vm) {
    for (const Nic& nic : vm.nics) {
        try {
            if (iface_exists(nic.host_dev)) {
                delete_link(nic.host_dev);
                info("removed " + nic.host_dev);
            }
        } catch (const std::exception& err) {
            warn(err.what());
        }
    }
    const std::string sock = api_socket(host, vm);
    if (::unlink(sock.c_str()) == 0) {
        info("removed socket " + sock);
    }
}

struct FdSet {
    std::vector<int> fds;
    ~FdSet() {
        for (int fd : fds) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }
    FdSet() = default;
    FdSet(const FdSet&) = delete;
    FdSet& operator=(const FdSet&) = delete;
};

void lock_writable_disks(const Vm& vm, FdSet& held) {
    for (const Disk& disk : vm.disks) {
        if (disk.readonly) {
            continue;
        }
        if (!exists(disk.path)) {
            fail("disk '" + disk.path + "' does not exist");
        }
        const int fd = ::open(disk.path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            fail("failed to open disk '" + disk.path + "': " + std::strerror(errno));
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            fail("disk '" + disk.path +
                 "' is locked by another process. Refusing to start to avoid filesystem corruption.");
        }
        held.fds.push_back(fd);
    }
}

void ensure_paths(const Vm& vm) {
    if (vm.kernel.empty()) {
        fail("VM '" + vm.name + "' is missing boot-source.kernel_image_path");
    }
    if (!exists(vm.kernel)) {
        fail("kernel '" + vm.kernel + "' does not exist");
    }
    if (!vm.initrd.empty() && !exists(vm.initrd)) {
        fail("initrd '" + vm.initrd + "' does not exist");
    }
    for (const Disk& disk : vm.disks) {
        if (!exists(disk.path)) {
            fail("disk '" + disk.path + "' does not exist");
        }
    }
}

volatile sig_atomic_t g_stop = 0;
pid_t g_child = 0;

void on_signal(int) {
    g_stop = 1;
    if (g_child > 0) {
        ::kill(g_child, SIGTERM);
    }
}

void cmd_prepare(const Host& host, const Vm& vm) {
    prepare_nics(host, vm);
}

void cmd_clean(const Host& host, const Vm& vm) {
    const std::string sock = api_socket(host, vm);
    if (api_alive(sock)) {
        fail("VM '" + vm.name + "' is still running");
    }
    remove_runtime(host, vm);
}

void cmd_stop(const Host& host, const Vm& vm) {
    const std::string sock = api_socket(host, vm);
    if (!api_alive(sock)) {
        info("VM '" + vm.name + "' is not running");
        return;
    }
    send_ctrl_alt_del(sock);
    wait_until_down(sock, host.stop_timeout_secs);
    info("VM '" + vm.name + "' stopped");
}

void cmd_run(const Host& host, Vm vm) {
    const Report report = scan(host, {vm.file});
    print_report(report);
    if (report.bad_vms.count(vm.name) != 0) {
        fail("refusing to start '" + vm.name + "' because resource check failed");
    }
    vm.file = real_path(vm.file);
    const std::string binary = find_firecracker(host);
    const std::string sock = api_socket(host, vm);
    if (api_alive(sock)) {
        fail_code(kExitAlreadyRunning, "VM '" + vm.name + "' is already running");
    }
    ensure_paths(vm);
    if (::mkdir(host.runtime_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        fail("cannot create '" + host.runtime_dir + "': " + std::strerror(errno));
    }
    prepare_nics(host, vm);
    struct CleanGuard {
        const Host* host = nullptr;
        const Vm* vm = nullptr;
        ~CleanGuard() {
            if (host != nullptr && vm != nullptr) {
                remove_runtime(*host, *vm);
            }
        }
    } guard{&host, &vm};
    FdSet locks;
    lock_writable_disks(vm, locks);
    ::unlink(sock.c_str());

    sigset_t block;
    sigset_t previous;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &previous);
    const pid_t pid = ::fork();
    if (pid < 0) {
        fail(std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &previous, nullptr);
        ::execl(binary.c_str(), binary.c_str(), "--api-sock", sock.c_str(), "--config-file",
                vm.file.c_str(), static_cast<char*>(nullptr));
        std::perror("fcctl: exec");
        _exit(127);
    }
    g_child = pid;
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
    sigprocmask(SIG_SETMASK, &previous, nullptr);

    info("starting " + vm.name + " (" + binary + ")");
    int status = 0;
    while (true) {
        const pid_t got = ::waitpid(pid, &status, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            fail(std::string("waitpid failed: ") + std::strerror(errno));
        }
        break;
    }
    g_child = 0;
    if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        info("firecracker exited with status " + std::to_string(code));
        if (code != 0) {
            // Cleanup still runs via guard, then the process exits non-zero.
            guard.host = nullptr;
            remove_runtime(host, vm);
            std::exit(code);
        }
        return;
    }
    if (WIFSIGNALED(status)) {
        const int code = 128 + WTERMSIG(status);
        info("firecracker killed by signal " + std::to_string(WTERMSIG(status)));
        guard.host = nullptr;
        remove_runtime(host, vm);
        std::exit(code);
    }
}

void write_unit_file(const std::string& dir, const Vm& vm, const std::string& text) {
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        fail("cannot create '" + dir + "': " + std::strerror(errno));
    }
    const std::string dest = dir + "/" + service_name(vm.name);
    const std::string tmp = dest + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            fail("cannot write '" + tmp + "'");
        }
        out << text;
    }
    if (::rename(tmp.c_str(), dest.c_str()) != 0) {
        ::unlink(tmp.c_str());
        fail("cannot replace '" + dest + "': " + std::strerror(errno));
    }
    info("wrote " + dest);
}

void cmd_generate_one(const Host& host, const Vm& vm, bool write) {
    const Report report = scan(host, {vm.file});
    print_report(report);
    if (report.bad_vms.count(vm.name) != 0) {
        fail("refusing to generate a unit for conflicting VM '" + vm.name + "'");
    }
    Vm copy = vm;
    copy.file = real_path(vm.file);
    const std::string text = render_unit(host, copy);
    if (write) {
        write_unit_file(host.unit_dir, copy, text);
    } else {
        std::cout << text;
    }
}

void cmd_generate_all(const Host& host, bool write) {
    if (!is_dir(host.config_dir)) {
        fail("VM directory '" + host.config_dir + "' does not exist");
    }
    const Report report = scan(host, {});
    print_report(report);
    std::vector<Vm> vms;
    Report ignored;
    load_dir(host.config_dir, vms, ignored);
    int wrote = 0;
    for (Vm vm : vms) {
        if (report.bad_vms.count(vm.name) != 0) {
            warn("skipping unit for conflicting VM '" + vm.name + "'");
            continue;
        }
        vm.file = real_path(vm.file);
        const std::string text = render_unit(host, vm);
        if (write) {
            write_unit_file(host.unit_dir, vm, text);
        } else {
            std::cout << text;
            if (text.empty() || text.back() != '\n') {
                std::cout << '\n';
            }
        }
        ++wrote;
    }
    if (wrote == 0 && !report.errors.empty()) {
        fail("no units generated");
    }
}

int cmd_generator(const Host& host, const std::string& out_dir) {
    if (!is_dir(host.config_dir)) {
        warn("VM directory '" + host.config_dir + "' does not exist");
        return 0;
    }
    const Report report = scan(host, {});
    print_report(report);
    std::vector<Vm> vms;
    Report ignored;
    load_dir(host.config_dir, vms, ignored);
    for (Vm vm : vms) {
        if (report.bad_vms.count(vm.name) != 0) {
            std::cerr << "[fcctl-generator] skipping conflicting VM '" << vm.name << "'\n";
            continue;
        }
        vm.file = real_path(vm.file);
        write_unit_file(out_dir, vm, render_unit(host, vm));
    }
    return report.errors.empty() ? 0 : 0;
}

Vm vm_from_arg(const std::string& target, const Host& host) {
    if (is_file(target)) {
        return parse_vm(target);
    }
    const std::string in_dir = host.config_dir + "/" + target + ".json";
    if (is_file(in_dir)) {
        return parse_vm(in_dir);
    }
    const std::string with_suffix = target + ".json";
    if (is_file(with_suffix)) {
        return parse_vm(with_suffix);
    }
    fail("cannot find VM config '" + target + "'");
}

void usage() {
    std::cerr
        << "fcctl " << kVersion
        << " — Firecracker controller\n"
           "\n"
           "  fcctl [--config|-c <host.json>] --run <vm.json>\n"
           "  fcctl [--config|-c <host.json>] --stop <vm.json>\n"
           "  fcctl [--config|-c <host.json>] --clean <vm.json>\n"
           "  fcctl [--config|-c <host.json>] --prepare <vm.json>\n"
           "  fcctl [--config|-c <host.json>] --check [vm.json]\n"
           "  fcctl [--config|-c <host.json>] --generate <vm|--all> [--write]\n"
           "\n"
           "VM files are Firecracker JSON in /etc/fcctl (config.json is the host file).\n"
           "Host network[] creates tap/bridge/macvtap/macvlan devices; no addresses or NAT.\n"
           "Subcommands (run, stop, clean, prepare, check, generate) are aliases for the flags.\n";
}

std::string action_token(const std::string& arg) {
    if (arg == "--run" || arg == "run") {
        return "run";
    }
    if (arg == "--stop" || arg == "stop") {
        return "stop";
    }
    if (arg == "--clean" || arg == "clean") {
        return "clean";
    }
    if (arg == "--prepare" || arg == "prepare") {
        return "prepare";
    }
    if (arg == "--check" || arg == "check") {
        return "check";
    }
    if (arg == "--generate" || arg == "generate") {
        return "generate";
    }
    if (arg == "systemd-generator") {
        return "systemd-generator";
    }
    return {};
}

struct Cli {
    std::string config;
    std::string action;
    std::string target;
    bool all = false;
    bool write = false;
};

void set_action(Cli& cli, const std::string& action) {
    if (!cli.action.empty() && cli.action != action) {
        fail("conflicting actions '" + cli.action + "' and '" + action + "'");
    }
    cli.action = action;
}

Cli parse_cli(int argc, char** argv) {
    Cli cli;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "help") {
            set_action(cli, "help");
            continue;
        }
        if (arg == "--version" || arg == "version") {
            set_action(cli, "version");
            continue;
        }
        if (arg == "self-test") {
            set_action(cli, "self-test");
            continue;
        }
        if (arg == "--all") {
            cli.all = true;
            continue;
        }
        if (arg == "--write") {
            cli.write = true;
            continue;
        }
        if (arg == "--config" || arg == "-c") {
            if (i + 1 >= argc) {
                fail(arg + " needs a path");
            }
            cli.config = argv[++i];
            continue;
        }
        if (arg.rfind("--config=", 0) == 0) {
            cli.config = arg.substr(9);
            if (cli.config.empty()) {
                fail("--config needs a path");
            }
            continue;
        }
        const std::string action = action_token(arg);
        if (!action.empty()) {
            set_action(cli, action);
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            fail("unknown option '" + arg + "'");
        }
        if (!cli.target.empty()) {
            fail("unexpected argument '" + arg + "'");
        }
        cli.target = arg;
    }
    return cli;
}

Host host_from_text(const std::string& text) {
    Host host;
    apply_host_json(host, parse_json(text), "test.json");
    return host;
}

void expect_host_fail(const std::string& text, const std::string& needle) {
    try {
        host_from_text(text);
        fail("expected host JSON to fail: " + text);
    } catch (const std::runtime_error& err) {
        if (std::string(err.what()).find(needle) == std::string::npos) {
            fail("wrong error '" + std::string(err.what()) + "', expected '" + needle + "'");
        }
    }
}

void self_test() {
    const Json sample = parse_json(R"({
        "boot-source": {"kernel_image_path": "/k", "boot_args": "console=ttyS0"},
        "drives": [{
            "drive_id": "rootfs",
            "path_on_host": "/dev/ubuntu-vg/debian.raw",
            "is_root_device": true,
            "is_read_only": false,
            "cache_type": "Writeback"
        }],
        "machine-config": {"vcpu_count": 2, "mem_size_mib": 2048, "smt": false},
        "network-interfaces": [{
            "iface_id": "eth0",
            "guest_mac": "0a:fc:00:00:02:00",
            "host_dev_name": "fc-debian"
        }]
    })");
    if (sample.find("drives") == nullptr || sample.find("drives")->a.size() != 1) {
        fail("parser dropped the drive");
    }
    if (systemd_path_escape("/dev/ubuntu-vg/debian.raw") != "dev-ubuntu\\x2dvg-debian.raw") {
        fail("systemd path escape mismatch: " + systemd_path_escape("/dev/ubuntu-vg/debian.raw"));
    }
    Vm debian;
    debian.name = "debian";
    debian.file = "/etc/fcctl/debian.json";
    debian.kernel = "/etc/fcctl/boot/vmlinux-exp";
    debian.disks.push_back({"/dev/ubuntu-vg/debian.raw", false});
    debian.nics.push_back({"eth0", "0a:fc:00:00:02:00", "fc-debian"});
    Host host;
    ::setenv("FCCTL_BIN", "/usr/local/bin/fcctl", 1);
    const std::string unit = render_unit(host, debian);
    if (unit.find("SyslogIdentifier=debian.fc\n") == std::string::npos ||
        unit.find("ExecStart=\"/usr/local/bin/fcctl\" --config \"/etc/fcctl/config.json\" "
                  "--run \"/etc/fcctl/debian.json\"\n") == std::string::npos ||
        unit.find("ExecStop=\"/usr/local/bin/fcctl\" --config \"/etc/fcctl/config.json\" "
                  "--stop \"/etc/fcctl/debian.json\"\n") == std::string::npos ||
        unit.find("ExecStopPost=-\"/usr/local/bin/fcctl\" --config \"/etc/fcctl/config.json\" "
                  "--clean \"/etc/fcctl/debian.json\"\n") == std::string::npos ||
        unit.find("Requires=dev-ubuntu\\x2dvg-debian.raw.device\n") == std::string::npos ||
        unit.find("RequiresMountsFor=/etc/fcctl/boot/vmlinux-exp\n") == std::string::npos ||
        unit.find("RestartPreventExitStatus=69\n") == std::string::npos) {
        fail("unit text mismatch:\n" + unit);
    }
    const Host bridged = host_from_text(R"({
        "network": [{ "type": "bridge", "master": "brg27" }]
    })");
    if (bridged.network.size() != 1 || bridged.network[0].type != "bridge" ||
        bridged.network[0].master != "brg27" || !bridged.network[0].name.empty()) {
        fail("failed to parse network[{type:bridge, master:brg27}]");
    }
    const Host legacy = host_from_text(R"({"bridge": "br0"})");
    if (legacy.network.size() != 1 || legacy.network[0].type != "bridge" ||
        legacy.network[0].master != "br0") {
        fail("legacy bridge key was not synthesized");
    }
    const Host mixed = host_from_text(R"({
        "network": [
            { "type": "tap" },
            { "type": "macvlan", "master": "eth0", "name": "fc-debian" }
        ]
    })");
    if (spec_for_nic(mixed, "fc-debian").type != "macvlan" ||
        spec_for_nic(mixed, "fc-other").type != "tap") {
        fail("network name matching is wrong");
    }
    const Host empty_net = host_from_text(R"({"network": []})");
    if (!empty_net.network.empty() || spec_for_nic(empty_net, "fc-debian").type != "tap") {
        fail("empty network[] should create plain taps");
    }
    expect_host_fail(R"({"network": [{ "type": "tap", "master": "brg27" }]})",
                     "tap does not take master");
    expect_host_fail(R"({"network": [{ "type": "wire" }]})", "type must be");
    expect_host_fail(R"({"network": [{ "type": "tap" }, { "type": "bridge", "master": "br0" }]})",
                     "only one default network entry");
    expect_host_fail(R"({"bridge": "br0", "network": [{ "type": "tap" }]})",
                     "set network or bridge, not both");
    expect_host_fail(R"({"network": [{ "type": "macvlan", "master": "eth0", "mode": "foo" }]})",
                     "mode must be");
    Vm rocky = debian;
    rocky.name = "rocky";
    rocky.file = "/etc/fcctl/rocky.json";
    rocky.nics[0].host_dev = "fc-rocky";
    Report bad = check_vms({debian, rocky});
    bool disk = false;
    bool mac = false;
    for (const std::string& err : bad.errors) {
        disk = disk || err.find("disk conflict:") != std::string::npos;
        mac = mac || err.find("MAC conflict:") != std::string::npos;
    }
    if (!disk || !mac || bad.bad_vms.count("debian") == 0) {
        fail("conflict check missed a shared disk or MAC");
    }
    rocky.disks[0].path = "/dev/ubuntu-vg/rocky.raw";
    rocky.nics[0].mac = "0a:fc:00:00:01:00";
    Report good = check_vms({debian, rocky});
    if (!good.errors.empty()) {
        fail("distinct VMs were reported as conflicting");
    }
    if (version_nums("firecracker-v1.16.1-x86_64").size() != 3 ||
        !newer_bin("firecracker-v1.18.0-x86_64", "firecracker-v1.16.1-x86_64") ||
        newer_bin("firecracker-v1.9.0-x86_64", "firecracker-v1.16.0-x86_64")) {
        fail("release version ordering is wrong");
    }
    info("self-test ok");
}

int cmd_check(const Host& host, const std::string& target) {
    std::vector<std::string> extra;
    if (!target.empty()) {
        extra.push_back(target);
    }
    if (!extra.empty() || is_dir(host.config_dir)) {
        const Report report = scan(host, extra);
        print_report(report);
        if (!report.errors.empty()) {
            return 1;
        }
        info(std::to_string([&] {
                 std::vector<Vm> vms;
                 Report ignored;
                 if (is_dir(host.config_dir)) {
                     load_dir(host.config_dir, vms, ignored);
                 }
                 return vms.size();
             }()) +
             " VM config(s) checked");
        return 0;
    }
    fail("VM directory '" + host.config_dir + "' does not exist");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::string arg0 = argc > 0 ? argv[0] : "";
        const auto slash = arg0.find_last_of('/');
        if (slash != std::string::npos) {
            arg0 = arg0.substr(slash + 1);
        }
        if (arg0.find("generator") != std::string::npos) {
            const Host host = load_host("");
            if (argc < 2 || !is_dir(argv[1])) {
                fail("generator expects the systemd output directory");
            }
            return cmd_generator(host, argv[1]);
        }
        if (argc < 2) {
            usage();
            return 1;
        }
        const Cli cli = parse_cli(argc, argv);
        if (cli.action == "help") {
            usage();
            return 0;
        }
        if (cli.action == "version") {
            std::cout << "fcctl " << kVersion << "\n";
            return 0;
        }
        if (cli.action == "self-test") {
            self_test();
            return 0;
        }
        if (cli.action.empty()) {
            usage();
            return 1;
        }
        if (cli.write && cli.action != "generate") {
            fail("--write is only valid with --generate");
        }
        if (cli.all && cli.action != "generate") {
            fail("--all is only valid with --generate");
        }
        const Host host = load_host(cli.config);
        if (cli.action == "check") {
            return cmd_check(host, cli.target);
        }
        if (cli.action == "generate") {
            if (cli.all) {
                cmd_generate_all(host, cli.write);
                return 0;
            }
            if (cli.target.empty()) {
                fail("generate needs a VM json path or --all");
            }
            cmd_generate_one(host, vm_from_arg(cli.target, host), cli.write);
            return 0;
        }
        if (cli.action == "run" || cli.action == "stop" || cli.action == "clean" ||
            cli.action == "prepare") {
            if (cli.target.empty()) {
                fail(cli.action + " needs one VM json path");
            }
            Vm vm = vm_from_arg(cli.target, host);
            if (cli.action == "run") {
                cmd_run(host, std::move(vm));
            } else if (cli.action == "stop") {
                cmd_stop(host, vm);
            } else if (cli.action == "clean") {
                cmd_clean(host, vm);
            } else {
                cmd_prepare(host, vm);
            }
            return 0;
        }
        if (cli.action == "systemd-generator") {
            if (cli.target.empty() || !is_dir(cli.target)) {
                fail("systemd-generator needs the output directory");
            }
            return cmd_generator(host, cli.target);
        }
        fail("unknown command '" + cli.action + "'");
    } catch (const ExitCode& err) {
        std::cerr << "fcctl: " << err.what() << "\n";
        return err.code;
    } catch (const std::exception& err) {
        std::cerr << "fcctl: " << err.what() << "\n";
        return 1;
    }
}
