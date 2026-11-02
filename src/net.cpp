#include "net.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint16_t kRtmNewlink = 16;
constexpr std::uint16_t kRtmDellink = 17;
constexpr std::uint16_t kNlmFRequest = 0x01;
constexpr std::uint16_t kNlmFAck = 0x04;
constexpr std::uint16_t kNlmsgError = 2;
constexpr std::size_t kNlmsgHdrlen = 16;
constexpr std::uint16_t kIflaIfname = 3;
constexpr std::uint16_t kIflaLink = 5;
constexpr std::uint16_t kIflaMaster = 10;
constexpr std::uint16_t kIflaLinkinfo = 18;
constexpr std::uint16_t kIflaInfoKind = 1;
constexpr std::uint16_t kIflaInfoData = 2;
constexpr std::uint16_t kIflaMacvlanMode = 1;
constexpr std::uint16_t kNlmFCreate = 0x400;
constexpr std::uint16_t kNlmFExcl = 0x200;
constexpr std::uint32_t kIfFlagUp = 1;
constexpr std::uint32_t kMacvlanPrivate = 1;
constexpr std::uint32_t kMacvlanVepa = 2;
constexpr std::uint32_t kMacvlanBridge = 4;
constexpr std::uint32_t kMacvlanPassthru = 8;

std::uint32_t next_seq() {
    static std::uint32_t seq = 1;
    return seq++;
}

[[noreturn]] void io_error(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

struct NlError : std::runtime_error {
    int err;
    NlError(int error, const std::string& message)
        : std::runtime_error(message), err(error) {}
};

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    int get() const { return fd_; }

private:
    int fd_;
};

void put16(std::vector<std::uint8_t>& buf, std::size_t at, std::uint16_t value) {
    std::memcpy(buf.data() + at, &value, 2);
}

void put32(std::vector<std::uint8_t>& buf, std::size_t at, std::uint32_t value) {
    std::memcpy(buf.data() + at, &value, 4);
}

class Msg {
public:
    Msg(std::uint16_t kind, std::uint16_t flags, std::uint32_t seq) : buf_(kNlmsgHdrlen, 0) {
        put16(buf_, 4, kind);
        put16(buf_, 6, flags);
        put32(buf_, 8, seq);
    }

    void ifinfo(std::int32_t index, std::uint32_t flags, std::uint32_t change) {
        const std::size_t start = buf_.size();
        buf_.resize(start + 16, 0);
        put32(buf_, start + 4, static_cast<std::uint32_t>(index));
        put32(buf_, start + 8, flags);
        put32(buf_, start + 12, change);
    }

    void align() {
        while (buf_.size() % 4 != 0) {
            buf_.push_back(0);
        }
    }

    void attr(std::uint16_t kind, const void* data, std::size_t len) {
        align();
        const std::size_t at = buf_.size();
        buf_.resize(at + 4 + len, 0);
        put16(buf_, at, static_cast<std::uint16_t>(4 + len));
        put16(buf_, at + 2, kind);
        if (len > 0 && data != nullptr) {
            std::memcpy(buf_.data() + at + 4, data, len);
        }
        align();
    }

    void begin_attr(std::uint16_t kind) {
        align();
        const std::size_t at = buf_.size();
        buf_.resize(at + 4, 0);
        put16(buf_, at + 2, kind);
        nest_.push_back(at);
    }

    void end_attr() {
        if (nest_.empty()) {
            throw std::runtime_error("netlink nested attribute underflow");
        }
        align();
        const std::size_t at = nest_.back();
        nest_.pop_back();
        put16(buf_, at, static_cast<std::uint16_t>(buf_.size() - at));
    }

    std::vector<std::uint8_t> finish() {
        if (!nest_.empty()) {
            throw std::runtime_error("netlink nested attribute still open");
        }
        align();
        put32(buf_, 0, static_cast<std::uint32_t>(buf_.size()));
        return buf_;
    }

private:
    std::vector<std::uint8_t> buf_;
    std::vector<std::size_t> nest_;
};

std::vector<std::uint8_t> c_string(const std::string& text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return bytes;
}

// Some(result) ends the read. Empty means this datagram had no ACK.
bool parse_ack(const std::uint8_t* buf, std::size_t len, int& err_out) {
    std::size_t off = 0;
    while (off + kNlmsgHdrlen <= len) {
        std::uint32_t nlen = 0;
        std::memcpy(&nlen, buf + off, 4);
        if (nlen < kNlmsgHdrlen || off + nlen > len) {
            break;
        }
        std::uint16_t kind = 0;
        std::memcpy(&kind, buf + off + 4, 2);
        if (kind == kNlmsgError) {
            if (nlen < kNlmsgHdrlen + 4) {
                throw std::runtime_error("short netlink error message");
            }
            std::int32_t err = 0;
            std::memcpy(&err, buf + off + 16, 4);
            err_out = err;
            return true;
        }
        off += (nlen + 3) & ~std::size_t{3};
    }
    return false;
}

class Netlink {
public:
    Netlink() {
        fd_ = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (fd_ < 0) {
            io_error("failed to open a netlink socket");
        }
        sockaddr_nl local{};
        local.nl_family = AF_NETLINK;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            const int saved = errno;
            ::close(fd_);
            errno = saved;
            io_error("failed to bind the netlink socket");
        }
        timeval timeout{};
        timeout.tv_sec = 2;
        if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            const int saved = errno;
            ::close(fd_);
            errno = saved;
            io_error("failed to set the netlink timeout");
        }
    }

    Netlink(const Netlink&) = delete;
    Netlink& operator=(const Netlink&) = delete;

    ~Netlink() { ::close(fd_); }

    void talk(const std::vector<std::uint8_t>& msg) {
        sockaddr_nl dest{};
        dest.nl_family = AF_NETLINK;
        if (::sendto(fd_, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&dest),
                     sizeof(dest)) < 0) {
            io_error("failed to send a netlink message");
        }
        std::uint8_t buf[8192];
        for (int attempt = 0; attempt < 8; ++attempt) {
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                io_error("failed to read a netlink reply");
            }
            int err = 0;
            if (!parse_ack(buf, static_cast<std::size_t>(n), err)) {
                continue;
            }
            if (err == 0) {
                return;
            }
            errno = -err;
            throw NlError(errno, std::string("netlink: ") + std::strerror(errno));
        }
        throw std::runtime_error("netlink reply did not include an acknowledgement");
    }

private:
    int fd_ = -1;
};

int ifindex_of(const std::string& name) {
    const std::string path = "/sys/class/net/" + name + "/ifindex";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error("network device '" + name + "' was not found");
    }
    Fd owned(fd);
    char buf[32] = {};
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        throw std::runtime_error("failed to read ifindex for '" + name + "'");
    }
    char* end = nullptr;
    const long value = std::strtol(buf, &end, 10);
    if (end == buf || value <= 0) {
        throw std::runtime_error("invalid ifindex for '" + name + "'");
    }
    return static_cast<int>(value);
}

void fill_ifr_name(ifreq& req, const std::string& name) {
    if (name.size() >= IFNAMSIZ) {
        throw std::runtime_error("interface name '" + name + "' does not fit in IFNAMSIZ");
    }
    std::memset(req.ifr_name, 0, IFNAMSIZ);
    std::memcpy(req.ifr_name, name.data(), name.size());
}

}  // namespace

bool iface_exists(const std::string& name) {
    const std::string path = "/sys/class/net/" + name;
    return ::access(path.c_str(), F_OK) == 0;
}

void create_tap(const std::string& name) {
    const int fd = ::open("/dev/net/tun", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        io_error("failed to open /dev/net/tun");
    }
    Fd owned(fd);
    ifreq req{};
    fill_ifr_name(req, name);
    req.ifr_flags = static_cast<short>(IFF_TAP | IFF_NO_PI);
    if (::ioctl(fd, TUNSETIFF, &req) < 0) {
        io_error("failed to create tap device '" + name + "'");
    }
    if (::ioctl(fd, TUNSETPERSIST, 1) < 0) {
        io_error("failed to persist tap device '" + name + "'");
    }
}

namespace {

std::uint32_t macvlan_mode_value(const std::string& mode) {
    const std::string& value = mode.empty() ? "bridge" : mode;
    if (value == "private") {
        return kMacvlanPrivate;
    }
    if (value == "vepa") {
        return kMacvlanVepa;
    }
    if (value == "bridge") {
        return kMacvlanBridge;
    }
    if (value == "passthru") {
        return kMacvlanPassthru;
    }
    throw std::runtime_error("unknown macvlan mode '" + mode + "'");
}

void create_mac_device(const std::string& name, const std::string& master, const std::string& kind,
                       const std::string& mode) {
    const int parent = ifindex_of(master);
    const std::uint32_t mode_value = macvlan_mode_value(mode);
    Msg msg(kRtmNewlink, kNlmFRequest | kNlmFAck | kNlmFCreate | kNlmFExcl, next_seq());
    msg.ifinfo(0, 0, 0);
    const std::vector<std::uint8_t> ifname = c_string(name);
    msg.attr(kIflaIfname, ifname.data(), ifname.size());
    const std::int32_t parent_index = parent;
    msg.attr(kIflaLink, &parent_index, sizeof(parent_index));
    msg.begin_attr(kIflaLinkinfo);
    const std::vector<std::uint8_t> kind_bytes = c_string(kind);
    msg.attr(kIflaInfoKind, kind_bytes.data(), kind_bytes.size());
    msg.begin_attr(kIflaInfoData);
    msg.attr(kIflaMacvlanMode, &mode_value, sizeof(mode_value));
    msg.end_attr();
    msg.end_attr();
    try {
        Netlink().talk(msg.finish());
    } catch (const std::runtime_error& err) {
        throw std::runtime_error("failed to create " + kind + " device '" + name + "' on '" +
                                 master + "': " + err.what());
    }
}

}  // namespace

void create_macvlan(const std::string& name, const std::string& master, const std::string& mode) {
    create_mac_device(name, master, "macvlan", mode);
}

void create_macvtap(const std::string& name, const std::string& master, const std::string& mode) {
    create_mac_device(name, master, "macvtap", mode);
}

void set_master(const std::string& name, const std::string& bridge) {
    const int index = ifindex_of(name);
    const int master = ifindex_of(bridge);
    Msg msg(kRtmNewlink, kNlmFRequest | kNlmFAck, next_seq());
    msg.ifinfo(index, 0, 0);
    const std::int32_t master_index = master;
    msg.attr(kIflaMaster, &master_index, sizeof(master_index));
    try {
        Netlink().talk(msg.finish());
    } catch (const std::runtime_error& err) {
        throw std::runtime_error("failed to attach '" + name + "' to bridge '" + bridge +
                                 "': " + err.what());
    }
}

void set_up(const std::string& name) {
    Msg msg(kRtmNewlink, kNlmFRequest | kNlmFAck, next_seq());
    msg.ifinfo(ifindex_of(name), kIfFlagUp, kIfFlagUp);
    try {
        Netlink().talk(msg.finish());
    } catch (const std::runtime_error& err) {
        throw std::runtime_error("failed to bring up interface '" + name + "': " + err.what());
    }
}

void delete_link(const std::string& name) {
    if (name.empty() || !iface_exists(name)) {
        return;
    }
    Msg msg(kRtmDellink, kNlmFRequest | kNlmFAck, next_seq());
    msg.ifinfo(0, 0, 0);
    const std::vector<std::uint8_t> bytes = c_string(name);
    msg.attr(kIflaIfname, bytes.data(), bytes.size());
    try {
        Netlink().talk(msg.finish());
    } catch (const NlError& err) {
        if (err.err == ENODEV) {
            return;
        }
        throw std::runtime_error("failed to delete interface '" + name + "': " + err.what());
    }
}
