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
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint16_t kRtmNewlink = 16;
constexpr std::uint16_t kRtmDellink = 17;
constexpr std::uint16_t kRtmGetlink = 18;
constexpr std::uint16_t kNlmFRequest = 0x01;
constexpr std::uint16_t kNlmFAck = 0x04;
constexpr std::uint16_t kNlmsgError = 2;
constexpr std::uint16_t kNlmsgDone = 3;
constexpr std::uint16_t kNlaTypeMask = 0x3fff;
constexpr std::size_t kNlmsgHdrlen = 16;
constexpr std::uint16_t kIflaAddress = 1;
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

// Pull RTM_NEWLINK messages out of one datagram. An ACK sets saw_ack.
// Messages whose sequence does not match the request are ignored.
void collect_replies(const std::uint8_t* buf, std::size_t len, std::uint32_t seq, int& err_out,
                     bool& saw_ack, std::vector<std::vector<std::uint8_t>>& out) {
    std::size_t remain = len;
    const std::uint8_t* cur = buf;
    while (remain >= kNlmsgHdrlen) {
        std::uint32_t nlen = 0;
        std::memcpy(&nlen, cur, 4);
        if (nlen < kNlmsgHdrlen || nlen > remain) {
            break;
        }
        std::uint16_t kind = 0;
        std::memcpy(&kind, cur + 4, 2);
        std::uint32_t msg_seq = 0;
        std::memcpy(&msg_seq, cur + 8, 4);
        if (msg_seq == seq) {
            if (kind == kNlmsgError) {
                if (nlen < kNlmsgHdrlen + 4) {
                    throw std::runtime_error("short netlink error message");
                }
                std::int32_t err = 0;
                std::memcpy(&err, cur + kNlmsgHdrlen, 4);
                err_out = err;
                saw_ack = true;
            } else if (kind == kRtmNewlink) {
                out.emplace_back(cur, cur + nlen);
            } else if (kind == kNlmsgDone) {
                saw_ack = true;
            }
        }
        const std::size_t step = (static_cast<std::size_t>(nlen) + 3) & ~std::size_t{3};
        if (step < kNlmsgHdrlen || step > remain) {
            break;
        }
        cur += step;
        remain -= step;
    }
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

    // Datagrams are kept until the ACK. A reply larger than the buffer is
    // received whole; GETLINK carries more attributes than an ACK.
    std::vector<std::vector<std::uint8_t>> roundtrip(const std::vector<std::uint8_t>& msg) {
        sockaddr_nl dest{};
        dest.nl_family = AF_NETLINK;
        if (::sendto(fd_, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&dest),
                     sizeof(dest)) < 0) {
            io_error("failed to send a netlink message");
        }
        std::uint32_t seq = 0;
        if (msg.size() >= 12) {
            std::memcpy(&seq, msg.data() + 8, 4);
        }
        std::vector<std::vector<std::uint8_t>> replies;
        std::vector<std::uint8_t> buf(8192);
        int spins = 0;
        while (spins < 32) {
            const ssize_t peeked = ::recv(fd_, buf.data(), buf.size(), MSG_PEEK | MSG_TRUNC);
            if (peeked < 0) {
                if (errno == EINTR) {
                    continue;
                }
                io_error("failed to read a netlink reply");
            }
            if (static_cast<std::size_t>(peeked) > buf.size()) {
                buf.resize(static_cast<std::size_t>(peeked));
                continue;
            }
            const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                io_error("failed to read a netlink reply");
            }
            ++spins;
            if (n == 0) {
                continue;
            }
            int err = 0;
            bool saw_ack = false;
            collect_replies(buf.data(), static_cast<std::size_t>(n), seq, err, saw_ack, replies);
            if (!saw_ack) {
                continue;
            }
            if (err == 0) {
                return replies;
            }
            errno = -err;
            throw NlError(errno, std::string("netlink: ") + std::strerror(errno));
        }
        throw std::runtime_error("netlink reply did not include an acknowledgement");
    }

    void talk(const std::vector<std::uint8_t>& msg) { roundtrip(msg); }

private:
    int fd_ = -1;
};

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Six colon-separated hex octets, the same shape as guest_mac.
void parse_mac(const std::string& mac, unsigned char out[6]) {
    if (mac.size() != 17) {
        throw std::runtime_error("MAC address '" + mac + "' must look like 0a:fc:00:00:02:00");
    }
    for (int i = 0; i < 6; ++i) {
        const std::size_t at = static_cast<std::size_t>(i) * 3;
        if (i > 0 && mac[at - 1] != ':') {
            throw std::runtime_error("MAC address '" + mac + "' must look like 0a:fc:00:00:02:00");
        }
        const int hi = hex_nibble(mac[at]);
        const int lo = hex_nibble(mac[at + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("MAC address '" + mac + "' must look like 0a:fc:00:00:02:00");
        }
        out[i] = static_cast<unsigned char>((hi << 4) | lo);
    }
}

bool wait_for_char_device(const std::string& path) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode)) {
            return true;
        }
        timespec pause{};
        pause.tv_nsec = 50L * 1000L * 1000L;
        ::nanosleep(&pause, nullptr);
    }
    return false;
}

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

template <typename Fn>
void for_each_nla(const std::uint8_t* data, std::size_t len, Fn fn) {
    std::size_t off = 0;
    while (off + 4 <= len) {
        std::uint16_t nlen = 0;
        std::uint16_t ntype = 0;
        std::memcpy(&nlen, data + off, 2);
        std::memcpy(&ntype, data + off + 2, 2);
        if (nlen < 4 || static_cast<std::size_t>(nlen) > len - off) {
            break;
        }
        fn(static_cast<std::uint16_t>(ntype & kNlaTypeMask), data + off + 4,
           static_cast<std::size_t>(nlen - 4));
        const std::size_t step = (static_cast<std::size_t>(nlen) + 3) & ~std::size_t{3};
        if (step < 4 || step > len - off) {
            break;
        }
        off += step;
    }
}

std::string nla_string(const std::uint8_t* data, std::size_t len) {
    while (len > 0 && data[len - 1] == 0) {
        --len;
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

// Kind string inside one RTM_NEWLINK. Empty when the device has no IFLA_INFO_KIND
// (a bridge or a physical NIC) or the buffer is not a NEWLINK.
std::string kind_in_newlink(const std::uint8_t* data, std::size_t len) {
    if (data == nullptr || len < kNlmsgHdrlen + 16) {
        return {};
    }
    std::uint16_t kind = 0;
    std::memcpy(&kind, data + 4, 2);
    if (kind != kRtmNewlink) {
        return {};
    }
    std::string found;
    for_each_nla(data + 32, len - 32,
                 [&](std::uint16_t type, const std::uint8_t* payload, std::size_t n) {
                     if (type != kIflaLinkinfo) {
                         return;
                     }
                     for_each_nla(payload, n,
                                  [&](std::uint16_t inner, const std::uint8_t* ip, std::size_t in) {
                                      if (inner == kIflaInfoKind) {
                                          found = nla_string(ip, in);
                                      }
                                  });
                 });
    return found;
}

std::string link_kind(const std::string& name) {
    Msg msg(kRtmGetlink, kNlmFRequest | kNlmFAck, next_seq());
    msg.ifinfo(ifindex_of(name), 0, 0);
    const std::vector<std::vector<std::uint8_t>> replies = Netlink().roundtrip(msg.finish());
    for (const std::vector<std::uint8_t>& reply : replies) {
        const std::string kind = kind_in_newlink(reply.data(), reply.size());
        if (!kind.empty()) {
            return kind;
        }
    }
    return {};
}

}  // namespace

bool iface_exists(const std::string& name) {
    const std::string path = "/sys/class/net/" + name;
    return ::access(path.c_str(), F_OK) == 0;
}

bool is_macvtap(const std::string& name) {
    if (!iface_exists(name)) {
        return false;
    }
    try {
        const std::string path = "/sys/class/macvtap/tap" + std::to_string(ifindex_of(name));
        return ::access(path.c_str(), F_OK) == 0;
    } catch (const std::exception&) {
        return false;
    }
}

bool is_tap(const std::string& name) {
    const std::string path = "/sys/class/net/" + name + "/tun_flags";
    return ::access(path.c_str(), F_OK) == 0;
}

bool is_macvlan(const std::string& name) {
    if (!iface_exists(name)) {
        return false;
    }
    try {
        return link_kind(name) == "macvlan";
    } catch (const std::exception&) {
        return false;
    }
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

void prepare_macvtap(const std::string& name, const std::string& mac) {
    unsigned char addr[6];
    parse_mac(mac, addr);
    const int index = ifindex_of(name);
    Msg msg(kRtmNewlink, kNlmFRequest | kNlmFAck, next_seq());
    msg.ifinfo(index, 0, 0);
    msg.attr(kIflaAddress, addr, sizeof(addr));
    try {
        Netlink().talk(msg.finish());
    } catch (const std::runtime_error& err) {
        throw std::runtime_error("failed to set MAC address of '" + name + "' to '" + mac +
                                 "': " + err.what());
    }

    const std::string path = "/dev/tap" + std::to_string(index);
    if (!wait_for_char_device(path)) {
        throw std::runtime_error("macvtap '" + name + "' has no device node " + path);
    }
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 && (errno == EACCES || errno == EPERM)) {
        if (::chown(path.c_str(), ::geteuid(), ::getegid()) != 0 ||
            ::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
            throw std::runtime_error("cannot make " + path + " openable: " + std::strerror(errno));
        }
        fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
        throw std::runtime_error("cannot open macvtap node " + path + ": " + std::strerror(errno));
    }
    ::close(fd);
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

namespace {

void put_u16(std::vector<std::uint8_t>& buf, std::size_t at, std::uint16_t value) {
    std::memcpy(buf.data() + at, &value, 2);
}

void put_u32(std::vector<std::uint8_t>& buf, std::size_t at, std::uint32_t value) {
    std::memcpy(buf.data() + at, &value, 4);
}

void expect_kind(const std::vector<std::uint8_t>& msg, const std::string& want) {
    const std::string got = kind_in_newlink(msg.data(), msg.size());
    if (got != want) {
        throw std::runtime_error("link kind parsed as '" + got + "', want '" + want + "'");
    }
}

}  // namespace

void net_self_test() {
    // nlmsghdr + ifinfomsg, then IFLA_LINKINFO (nested flag set) / IFLA_INFO_KIND "macvlan".
    std::vector<std::uint8_t> nested(48, 0);
    put_u32(nested, 0, 48);
    put_u16(nested, 4, 16);
    put_u16(nested, 32, 16);
    put_u16(nested, 34, static_cast<std::uint16_t>(18 | 0x8000));
    put_u16(nested, 36, 12);
    put_u16(nested, 38, 1);
    std::memcpy(nested.data() + 40, "macvlan", 7);
    expect_kind(nested, "macvlan");

    // IFLA_IFNAME is 9 bytes and pads to 12. Kind follows at the aligned offset.
    std::vector<std::uint8_t> padded(60, 0);
    put_u32(padded, 0, 60);
    put_u16(padded, 4, 16);
    put_u16(padded, 32, 9);
    put_u16(padded, 34, 3);
    std::memcpy(padded.data() + 36, "eth0", 4);
    put_u16(padded, 44, 16);
    put_u16(padded, 46, 18);
    put_u16(padded, 48, 12);
    put_u16(padded, 50, 1);
    std::memcpy(padded.data() + 52, "macvlan", 7);
    expect_kind(padded, "macvlan");

    std::memcpy(nested.data() + 40, "macvtap", 7);
    expect_kind(nested, "macvtap");

    std::vector<std::uint8_t> bare(32, 0);
    put_u32(bare, 0, 32);
    put_u16(bare, 4, 16);
    expect_kind(bare, "");

    put_u16(nested, 4, 17);
    expect_kind(nested, "");
}
