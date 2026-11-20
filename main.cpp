// FastGithubCpp - Windows GitHub accelerator (transparent / service / console)
// Build: see build.bat. Requires WinDivert 2.2.2-A SDK in D:\dev\WinDivert-2.2.2-A.
//
// Modes:
//   FastGithubCpp.exe                 run in console
//   FastGithubCpp.exe --engine        run engine without console banner (used by UI)
//   FastGithubCpp.exe start           install and start Windows service (admin)
//   FastGithubCpp.exe stop            stop and delete Windows service (admin)
//   FastGithubCpp.exe --service       internal service entry point
//   FastGithubCpp.exe --selftest host DNS + IP ranking self test

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windivert.h>

using Clock = std::chrono::steady_clock;
static std::atomic<bool> g_engine_stop{false};
static std::atomic<HANDLE> g_dns_divert{INVALID_HANDLE_VALUE};
static std::atomic<HANDLE> g_tcp_divert{INVALID_HANDLE_VALUE};
static std::atomic<uint64_t> g_bytes_rx{0};
static std::atomic<uint64_t> g_bytes_tx{0};

struct StatsShared {
    volatile LONG64 timestamp_ms;
    volatile LONG64 rx;
    volatile LONG64 tx;
};
static const char* STATS_MAP_NAME = "Local\\FastGithubCppStats";
static HANDLE g_stats_map = nullptr;
static void* g_stats_view = nullptr;

extern "C" void WINAPI DnsFlushResolverCache(void);

static BOOL WINAPI ctrl_handler(DWORD) {
    g_engine_stop.store(true);
    return TRUE;
}

struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};

struct Config {
    uint16_t port = 38457;
    uint16_t redirect_https_port = 38443;
    uint16_t redirect_ssh_port = 38422;
    uint16_t redirect_git_port = 38418;
    uint16_t redirect_http_port = 38480;
    std::vector<std::string> dns_servers = {"8.8.8.8", "119.29.29.29", "114.114.114.114"};
    std::vector<std::string> domains;
    bool ipv6 = true;
    bool verbose = false;
    int dns_timeout_ms = 1500;
    int connect_timeout_ms = 3000;
    int upstream_timeout_ms = 10000;
};

static std::string default_domains() {
    return "github.com;www.github.com;api.github.com;gist.github.com;raw.githubusercontent.com;"
           "githubusercontent.com;github.io;githubapp.com;githubassets.com;githubstatus.com;"
           "vscode-auth.github.com;codeload.github.com;objects.githubusercontent.com;"
           "avatars.githubusercontent.com;release-assets.githubusercontent.com;uploads.github.com;"
           "pipelines.actions.githubusercontent.com";
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string exe_dir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? "." : s.substr(0, p);
}

static void load_config_file(Config& c) {
    std::ifstream in(exe_dir() + "\\config.ini");
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k == "port") c.port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (k == "redirect_https_port") c.redirect_https_port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (k == "redirect_ssh_port") c.redirect_ssh_port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (k == "redirect_git_port") c.redirect_git_port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (k == "redirect_http_port") c.redirect_http_port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (k == "dns") c.dns_servers = split(v, ';');
        else if (k == "domains") c.domains = split(v, ';');
        else if (k == "ipv6") c.ipv6 = (v == "1" || v == "true" || v == "yes");
        else if (k == "verbose") c.verbose = (v == "1" || v == "true" || v == "yes");
        else if (k == "dns_timeout_ms") c.dns_timeout_ms = std::atoi(v.c_str());
        else if (k == "connect_timeout_ms") c.connect_timeout_ms = std::atoi(v.c_str());
        else if (k == "upstream_timeout_ms") c.upstream_timeout_ms = std::atoi(v.c_str());
    }
}

static void log_line(const Config& c, const std::string& msg) {
    if (!c.verbose) return;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count() % 86400000;
    std::ostringstream os;
    os << "[" << std::setfill('0') << std::setw(2) << (ms / 3600000) << ":"
       << std::setw(2) << ((ms / 60000) % 60) << ":" << std::setw(2) << ((ms / 1000) % 60)
       << "." << std::setw(3) << (ms % 1000) << "] " << msg;
    std::cout << os.str() << std::endl;
    std::string logpath = exe_dir() + "\\fastgithubcpp.log";
    std::ifstream chk(logpath, std::ios::ate | std::ios::binary);
    if (chk && chk.tellg() > static_cast<std::streamoff>(2 * 1024 * 1024)) {
        chk.close();
        std::ofstream trunc(logpath, std::ios::trunc | std::ios::binary);
    }
    std::ofstream f(logpath, std::ios::app);
    if (f) f << os.str() << "\n";
}

// ---------- socket helpers ----------

static void set_timeout(SOCKET s, int ms) {
    DWORD v = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
}

static bool fill_sockaddr(const std::string& ip, uint16_t port, sockaddr_storage& ss, int& len) {
    std::memset(&ss, 0, sizeof(ss));
    if (ip.find(':') != std::string::npos) {
        auto& a = reinterpret_cast<sockaddr_in6&>(ss);
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        if (inet_pton(AF_INET6, ip.c_str(), &a.sin6_addr) != 1) return false;
        len = sizeof(sockaddr_in6);
    } else {
        auto& a = reinterpret_cast<sockaddr_in&>(ss);
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
        len = sizeof(sockaddr_in);
    }
    return true;
}

static SOCKET connect_addr(const sockaddr_storage& ss, int len, int timeout_ms, long long* elapsed = nullptr) {
    auto start = Clock::now();
    SOCKET s = ::socket(ss.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    u_long nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);
    int rc = ::connect(s, reinterpret_cast<const sockaddr*>(&ss), len);
    bool ok = false;
    if (rc == 0) ok = true;
    else {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            fd_set w;
            FD_ZERO(&w);
            FD_SET(s, &w);
            timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            int n = ::select(0, nullptr, &w, nullptr, &tv);
            if (n > 0) {
                int soerr = 0;
                int elen = sizeof(soerr);
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &elen);
                ok = (soerr == 0);
            }
        }
    }
    if (elapsed) *elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (!ok) { closesocket(s); return INVALID_SOCKET; }
    u_long block = 0;
    ioctlsocket(s, FIONBIO, &block);
    return s;
}

static SOCKET connect_ip(const std::string& ip, uint16_t port, int timeout_ms, long long* elapsed = nullptr) {
    sockaddr_storage ss;
    int len = 0;
    if (!fill_sockaddr(ip, port, ss, len)) return INVALID_SOCKET;
    return connect_addr(ss, len, timeout_ms, elapsed);
}

static bool resolve_system(const std::string& host, uint16_t port, sockaddr_storage& out, int& outlen) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    std::string p = std::to_string(port);
    if (::getaddrinfo(host.c_str(), p.c_str(), &hints, &res) != 0 || !res) return false;
    std::memcpy(&out, res->ai_addr, res->ai_addrlen);
    outlen = static_cast<int>(res->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

static bool recv_exact(SOCKET s, char* buf, int n, int timeout_ms) {
    set_timeout(s, timeout_ms);
    int got = 0;
    while (got < n) {
        int r = ::recv(s, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

// ---------- DNS wire format ----------

static uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
static void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xff));
}

static std::vector<uint8_t> make_dns_query(uint16_t id, const std::string& host, int qtype) {
    std::vector<uint8_t> v;
    put16(v, id);
    put16(v, 0x0100);
    put16(v, 1);
    put16(v, 0); put16(v, 0); put16(v, 0);
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        size_t end = (dot == std::string::npos) ? host.size() : dot;
        size_t len = end - start;
        if (len > 63) break;
        v.push_back(static_cast<uint8_t>(len));
        v.insert(v.end(), host.begin() + start, host.begin() + end);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    v.push_back(0);
    put16(v, static_cast<uint16_t>(qtype));
    put16(v, 1);
    return v;
}

static size_t skip_name(const std::vector<uint8_t>& b, size_t pos, std::string* name) {
    size_t p = pos, endpos = 0;
    bool jumped = false;
    int hops = 0;
    while (p < b.size() && hops++ < 32) {
        uint8_t l = b[p];
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= b.size()) break;
            if (!jumped) { endpos = p + 2; jumped = true; }
            p = ((static_cast<size_t>(l & 0x3F) << 8) | b[p + 1]);
            continue;
        }
        if (l == 0) { p++; break; }
        if (name && p + 1 + l <= b.size()) {
            name->append(reinterpret_cast<const char*>(b.data() + p + 1), l);
            name->push_back('.');
        }
        p += l + 1;
    }
    return jumped ? endpos : p;
}

struct DnsRecord { std::string ip; int ttl; };

static std::vector<DnsRecord> parse_dns_response(const std::vector<uint8_t>& b, uint16_t id, int qtype) {
    std::vector<DnsRecord> out;
    if (b.size() < 12) return out;
    if (be16(b.data()) != id) return out;
    if (!(be16(b.data() + 2) & 0x8000)) return out;
    int qd = be16(b.data() + 4);
    int an = be16(b.data() + 6);
    size_t p = 12;
    for (int i = 0; i < qd; ++i) {
        p = skip_name(b, p, nullptr);
        if (p + 4 > b.size()) return out;
        p += 4;
    }
    for (int i = 0; i < an; ++i) {
        p = skip_name(b, p, nullptr);
        if (p + 10 > b.size()) break;
        int type = be16(b.data() + p);
        int cls = be16(b.data() + p + 2);
        int ttl = static_cast<int>(be32(b.data() + p + 4));
        int rdlen = be16(b.data() + p + 8);
        p += 10;
        if (p + rdlen > b.size()) break;
        if (type == qtype && cls == 1) {
            if (qtype == 1 && rdlen == 4) {
                std::ostringstream os;
                os << static_cast<int>(b[p]) << '.' << static_cast<int>(b[p + 1]) << '.'
                   << static_cast<int>(b[p + 2]) << '.' << static_cast<int>(b[p + 3]);
                out.push_back({os.str(), ttl});
            } else if (qtype == 28 && rdlen == 16) {
                char buf[INET6_ADDRSTRLEN] = {0};
                if (inet_ntop(AF_INET6, b.data() + p, buf, sizeof(buf))) out.push_back({buf, ttl});
            }
        }
        p += rdlen;
    }
    return out;
}

static bool dns_exchange(const std::string& server, const std::vector<uint8_t>& q,
                         std::vector<uint8_t>& resp, int timeout_ms) {
    sockaddr_storage ss;
    int len = 0;
    if (!fill_sockaddr(server, 53, ss, len)) return false;

    // Force TCP:53. Our own WinDivert interceptor only rewrites UDP:53 queries,
    // so TCP queries bypass it and we never resolve GitHub to 127.0.0.1 ourselves.
    SOCKET t = connect_addr(ss, len, timeout_ms);
    if (t == INVALID_SOCKET) return false;
    unsigned char lenbuf[2] = {static_cast<unsigned char>(q.size() >> 8),
                               static_cast<unsigned char>(q.size() & 0xff)};
    if (::send(t, reinterpret_cast<const char*>(lenbuf), 2, 0) != 2 ||
        ::send(t, reinterpret_cast<const char*>(q.data()), static_cast<int>(q.size()), 0) != static_cast<int>(q.size())) {
        closesocket(t); return false;
    }
    if (!recv_exact(t, reinterpret_cast<char*>(lenbuf), 2, timeout_ms)) { closesocket(t); return false; }
    int rlen = (lenbuf[0] << 8) | lenbuf[1];
    if (rlen <= 0 || rlen > 65535) { closesocket(t); return false; }
    std::vector<char> raw(rlen);
    bool ok = recv_exact(t, raw.data(), rlen, timeout_ms);
    closesocket(t);
    if (ok) resp.assign(reinterpret_cast<uint8_t*>(raw.data()), reinterpret_cast<uint8_t*>(raw.data()) + rlen);
    return ok;
}

struct IpCacheEntry { std::vector<std::string> ips; uint64_t expire; };
static std::mutex g_dns_mu;
static std::unordered_map<std::string, IpCacheEntry> g_dns_cache;

static std::vector<std::string> resolve_dns(const std::string& host, const Config& c) {
    std::string key = host + (c.ipv6 ? "/v6" : "/v4");
    uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now().time_since_epoch()).count());
    {
        std::lock_guard<std::mutex> lk(g_dns_mu);
        auto it = g_dns_cache.find(key);
        if (it != g_dns_cache.end() && it->second.expire > now && !it->second.ips.empty()) return it->second.ips;
    }

    std::vector<DnsRecord> records;
    int best_ttl = 600;
    static std::atomic<uint16_t> next_id{1};
    for (const auto& server : c.dns_servers) {
        std::vector<int> types = c.ipv6 ? std::vector<int>{1, 28} : std::vector<int>{1};
        for (int qt : types) {
            uint16_t id = next_id.fetch_add(1);
            auto q = make_dns_query(id, host, qt);
            std::vector<uint8_t> resp;
            if (!dns_exchange(server, q, resp, c.dns_timeout_ms)) continue;
            auto got = parse_dns_response(resp, id, qt);
            for (auto& r : got) best_ttl = std::min(best_ttl, r.ttl);
            records.insert(records.end(), got.begin(), got.end());
        }
    }

    std::vector<std::string> ips;
    for (auto& r : records) if (std::find(ips.begin(), ips.end(), r.ip) == ips.end()) ips.push_back(r.ip);

    if (ips.empty()) {
        addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0) {
            for (addrinfo* ai = res; ai; ai = ai->ai_next) {
                char buf[INET6_ADDRSTRLEN] = {0};
                void* ptr = ai->ai_family == AF_INET
                    ? reinterpret_cast<void*>(&reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr)
                    : reinterpret_cast<void*>(&reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr);
                if (inet_ntop(ai->ai_family, ptr, buf, sizeof(buf))) {
                    std::string ip(buf);
                    if (std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
                }
            }
            freeaddrinfo(res);
        }
        best_ttl = 60;
    }

    best_ttl = std::max(30, std::min(best_ttl, 600));
    {
        std::lock_guard<std::mutex> lk(g_dns_mu);
        g_dns_cache[key] = {ips, now + static_cast<uint64_t>(best_ttl)};
    }
    return ips;
}

// ---------- IP ranking ----------

struct SpeedCacheEntry { std::vector<std::string> ips; uint64_t expire; };
static std::mutex g_speed_mu;
static std::unordered_map<std::string, SpeedCacheEntry> g_speed_cache;

static std::vector<std::string> rank_ips(const std::string& host, uint16_t port,
                                         const std::vector<std::string>& fresh, const Config& c) {
    uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        Clock::now().time_since_epoch()).count());
    std::vector<std::string> candidates = fresh;
    {
        std::lock_guard<std::mutex> lk(g_speed_mu);
        auto it = g_speed_cache.find(host);
        if (it != g_speed_cache.end() && it->second.expire > now)
            for (auto& ip : it->second.ips)
                if (std::find(candidates.begin(), candidates.end(), ip) == candidates.end()) candidates.push_back(ip);
    }
    if (candidates.empty()) return {};

    struct Result { std::string ip; long long ms; bool ok; };
    std::vector<Result> results(candidates.size());
    std::vector<std::thread> workers;
    for (size_t i = 0; i < candidates.size(); ++i) {
        workers.emplace_back([&, i] {
            long long ms = -1;
            SOCKET s = connect_ip(candidates[i], port, c.connect_timeout_ms, &ms);
            if (s != INVALID_SOCKET) closesocket(s); else ms = -1;
            results[i] = {candidates[i], ms, ms >= 0};
        });
    }
    for (auto& t : workers) t.join();

    std::vector<std::pair<long long, std::string>> good;
    for (auto& r : results) if (r.ok) good.emplace_back(r.ms, r.ip);
    std::sort(good.begin(), good.end());
    std::vector<std::string> ranked;
    for (auto& g : good) ranked.push_back(g.second);
    {
        std::lock_guard<std::mutex> lk(g_speed_mu);
        g_speed_cache[host] = {ranked, now + 300};
    }
    return ranked;
}

static bool domain_match(const std::string& host, const std::vector<std::string>& patterns) {
    std::string h = host;
    while (!h.empty() && h.back() == '.') h.pop_back();
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (auto p : patterns) {
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        p = trim(p);
        if (p.rfind("*.", 0) == 0) p = p.substr(2);
        if (!p.empty() && (h == p || ends_with(h, "." + p))) return true;
    }
    return false;
}

static bool parse_host_port(const std::string& target, std::string& host, uint16_t& port) {
    if (target.empty()) return false;
    if (target[0] == '[') {
        size_t rb = target.find(']');
        if (rb == std::string::npos || rb + 1 >= target.size() || target[rb + 1] != ':') return false;
        host = target.substr(1, rb - 1);
        port = static_cast<uint16_t>(std::atoi(target.c_str() + rb + 2));
    } else {
        size_t colon = target.rfind(':');
        if (colon == std::string::npos) { host = target; port = 80; }
        else { host = target.substr(0, colon); port = static_cast<uint16_t>(std::atoi(target.c_str() + colon + 1)); }
    }
    return !host.empty() && port != 0;
}

struct Upstream {
    SOCKET sock = INVALID_SOCKET;
    std::string ip;
    std::string initial;  // first bytes from upstream, must be forwarded before relay
};

static Upstream connect_upstream(const std::string& host, uint16_t port, const Config& c) {
    Upstream r;
    bool numeric = inet_pton(AF_INET, host.c_str(), nullptr) == 1 ||
                   inet_pton(AF_INET6, host.c_str(), nullptr) == 1;
    if (numeric) {
        SOCKET s = connect_ip(host, port, c.upstream_timeout_ms);
        if (s != INVALID_SOCKET) { r.sock = s; r.ip = host; }
        return r;
    }
    if (!domain_match(host, c.domains)) {
        sockaddr_storage ss; int len = 0;
        if (resolve_system(host, port, ss, len)) {
            r.sock = connect_addr(ss, len, c.upstream_timeout_ms);
            if (r.sock != INVALID_SOCKET) r.ip = host;
        }
        return r;
    }

    auto fresh = resolve_dns(host, c);
    log_line(c, host + " DNS -> " + [&] {
        std::string s;
        for (auto& ip : fresh) { if (!s.empty()) s += ", "; s += ip; }
        return s;
    }());
    auto ranked = rank_ips(host, port, fresh, c);
    if (ranked.empty()) ranked = fresh;
    for (size_t i = 0; i < ranked.size() && i < 12; ++i) {
        SOCKET s = connect_ip(ranked[i], port, c.upstream_timeout_ms);
        if (s != INVALID_SOCKET) {
            r.sock = s; r.ip = ranked[i];
            log_line(c, host + " -> " + r.ip + ":" + std::to_string(port) + " (candidate #" + std::to_string(i + 1) + ")");
            return r;
        }
    }
    return r;
}

// Validate a candidate by sending the buffered client bytes and waiting for
// the first upstream byte. TCP-only speed tests pick CDN IPs that accept TCP
// but immediately fail TLS; this filters those out.
static Upstream connect_upstream_validated(const std::string& host, uint16_t port,
                                           const std::string& prefix, const Config& c) {
    Upstream r;
    bool numeric = inet_pton(AF_INET, host.c_str(), nullptr) == 1 ||
                   inet_pton(AF_INET6, host.c_str(), nullptr) == 1;
    std::vector<std::string> candidates;
    if (numeric) {
        candidates.push_back(host);
    } else if (!domain_match(host, c.domains)) {
        sockaddr_storage ss; int len = 0;
        if (resolve_system(host, port, ss, len)) {
            r.sock = connect_addr(ss, len, c.upstream_timeout_ms);
            if (r.sock != INVALID_SOCKET) r.ip = host;
        }
        return r;
    } else {
        auto fresh = resolve_dns(host, c);
        log_line(c, host + " DNS -> " + [&] {
            std::string s;
            for (auto& ip : fresh) { if (!s.empty()) s += ", "; s += ip; }
            return s;
        }());
        candidates = rank_ips(host, port, fresh, c);
        if (candidates.empty()) candidates = fresh;
    }

    int failed = 0;
    for (size_t i = 0; i < candidates.size() && i < 12; ++i) {
        SOCKET s = connect_ip(candidates[i], port, c.upstream_timeout_ms);
        if (s == INVALID_SOCKET) { ++failed; continue; }
        if (!prefix.empty())
            ::send(s, prefix.data(), static_cast<int>(prefix.size()), 0);

        std::string first;
        bool usable = true;
        if (port == 443 && !prefix.empty()) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(s, &rd);
            timeval tv{1, 200000};  // 1.2s
            int n = ::select(0, &rd, nullptr, nullptr, &tv);
            if (n > 0) {
                char buf[4096];
                int got = ::recv(s, buf, sizeof(buf), 0);
                if (got > 0) first.assign(buf, got);
                else usable = false;
            } else {
                usable = false;
            }
        }
        if (!usable) {
            closesocket(s);
            ++failed;
            continue;
        }
        r.sock = s;
        r.ip = candidates[i];
        r.initial = std::move(first);
        log_line(c, host + " -> " + r.ip + ":" + std::to_string(port) + " (validated)");
        return r;
    }
    if (failed) log_line(c, host + " all " + std::to_string(failed) + " candidates failed");
    return r;
}

static void relay(SOCKET a, SOCKET b, const Config& c) {
    (void)c;
    auto pump = [](SOCKET from, SOCKET to, bool upload) {
        std::vector<char> buf(16384);
        while (true) {
            int n = ::recv(from, buf.data(), static_cast<int>(buf.size()), 0);
            if (n <= 0) break;
            int off = 0;
            while (off < n) {
                int sent = ::send(to, buf.data() + off, n - off, 0);
                if (sent <= 0) return;
                off += sent;
            }
            if (upload) g_bytes_tx.fetch_add(static_cast<uint64_t>(n));
            else g_bytes_rx.fetch_add(static_cast<uint64_t>(n));
        }
    };
    std::thread t1([&] { pump(a, b, true); shutdown(b, SD_BOTH); shutdown(a, SD_BOTH); });
    std::thread t2([&] { pump(b, a, false); shutdown(a, SD_BOTH); shutdown(b, SD_BOTH); });
    t1.join(); t2.join();
}

static void send_text(SOCKET s, const std::string& text) {
    ::send(s, text.data(), static_cast<int>(text.size()), 0);
}

// ---------- transparent helpers ----------

static bool extract_sni(const std::vector<uint8_t>& d, std::string& host) {
    if (d.size() < 5 || d[0] != 0x16) return false;
    size_t recLen = be16(d.data() + 3);
    if (recLen < 4 || d.size() < 5 + recLen) return false;
    size_t hs = 5;
    if (d[hs] != 0x01) return false;
    uint32_t hsLen = (static_cast<uint32_t>(d[hs + 1]) << 16) | (static_cast<uint32_t>(d[hs + 2]) << 8) | d[hs + 3];
    if (hs + 4 + hsLen > 5 + recLen) return false;
    size_t p = hs + 4 + 2 + 32;
    if (p >= d.size()) return false;
    uint8_t sidLen = d[p++]; p += sidLen;
    if (p + 2 > d.size()) return false;
    uint16_t csLen = be16(d.data() + p); p += 2 + csLen;
    if (p >= d.size()) return false;
    uint8_t compLen = d[p++]; p += compLen;
    if (p + 2 > d.size()) return false;
    uint16_t extLen = be16(d.data() + p); p += 2;
    size_t end = p + extLen;
    while (p + 4 <= end && p + 4 <= d.size()) {
        uint16_t type = be16(d.data() + p);
        uint16_t len = be16(d.data() + p + 2);
        p += 4;
        if (type == 0x0000 && len >= 5 && p + len <= d.size()) {
            size_t r = p + 2;
            if (r + 3 <= p + len) {
                uint8_t nt = d[r++];
                uint16_t nl = be16(d.data() + r); r += 2;
                if (nt == 0 && nl > 0 && r + nl <= p + len) {
                    host.assign(reinterpret_cast<const char*>(d.data() + r), nl);
                    return true;
                }
            }
        }
        p += len;
    }
    return false;
}

static bool extract_http_host(const std::string& s, std::string& host) {
    size_t i = 0;
    while (i + 5 < s.size()) {
        size_t e = s.find("\r\n", i);
        if (e == std::string::npos) break;
        std::string line = s.substr(i, e - i);
        if (line.size() >= 5) {
            std::string k = line.substr(0, 5);
            std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (k == "host:") {
                host = trim(line.substr(5));
                size_t colon = host.rfind(':');
                if (colon != std::string::npos && host.find(']') == std::string::npos) host = host.substr(0, colon);
                return !host.empty();
            }
        }
        i = e + 2;
        if (line.empty()) break;
    }
    return false;
}

static std::vector<uint8_t> make_loopback_dns_response(const std::vector<uint8_t>& query) {
    std::vector<uint8_t> resp;
    if (query.size() < 12) return resp;
    size_t qend = skip_name(query, 12, nullptr);
    if (qend + 4 > query.size()) return resp;
    int qtype = be16(query.data() + qend);
    resp.assign(query.begin(), query.begin() + qend + 4);
    resp[2] = 0x81; resp[3] = 0x80;
    resp[4] = 0x00; resp[5] = 0x01;
    resp[6] = 0x00; resp[7] = 0x00;
    resp[8] = resp[9] = resp[10] = resp[11] = 0;
    if (qtype == 1) {
        resp[6] = 0x00; resp[7] = 0x01;
        put16(resp, 0xC00C);
        put16(resp, 1);
        put16(resp, 1);
        put16(resp, 0x01);
        put16(resp, 0x2C);
        put16(resp, 4);
        resp.push_back(127); resp.push_back(0); resp.push_back(0); resp.push_back(1);
    }
    return resp;  // AAAA: NOERROR with no answer, forces IPv4
}

static void dns_intercept_thread(const Config& c, std::atomic<bool>& stop) {
    HANDLE h = WinDivertOpen("udp.DstPort == 53", WINDIVERT_LAYER_NETWORK, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        log_line(c, "WinDivert DNS handle failed (need administrator). WinDivert error=" + std::to_string(GetLastError()));
        return;
    }
    g_dns_divert.store(h);
    log_line(c, "WinDivert DNS interceptor started");
    std::vector<uint8_t> packet(65535);
    while (!stop.load()) {
        UINT recvLen = 0;
        WINDIVERT_ADDRESS addr{};
        if (!WinDivertRecv(h, packet.data(), static_cast<UINT>(packet.size()), &recvLen, &addr)) continue;

        WINDIVERT_IPHDR* iph = nullptr;
        WINDIVERT_IPV6HDR* ip6 = nullptr;
        WINDIVERT_UDPHDR* udph = nullptr;
        void* data = nullptr;
        UINT dataLen = 0;
        bool handled = false;
        if (WinDivertHelperParsePacket(packet.data(), recvLen, &iph, &ip6, nullptr, nullptr, nullptr, nullptr,
                                       &udph, &data, &dataLen, nullptr, nullptr) && udph && data && dataLen >= 12) {
            auto* payload = static_cast<UINT8*>(data);
            std::vector<uint8_t> query(payload, payload + dataLen);
            if (!(query[2] & 0x80) && be16(query.data() + 4) >= 1) {
                std::string name;
                size_t qend = skip_name(query, 12, &name);
                if (!name.empty() && name.back() == '.') name.pop_back();
                if (qend + 4 <= query.size() && domain_match(name, c.domains)) {
                    auto resp = make_loopback_dns_response(query);
                    if (!resp.empty()) {
                        UINT newLen = static_cast<UINT>((iph ? sizeof(WINDIVERT_IPHDR) : sizeof(WINDIVERT_IPV6HDR))
                                                        + sizeof(WINDIVERT_UDPHDR) + resp.size());
                        if (iph) {
                            std::swap(iph->SrcAddr, iph->DstAddr);
                            iph->Length = htons(static_cast<u_short>(newLen));
                        } else if (ip6) {
                            for (int k = 0; k < 4; ++k) std::swap(ip6->SrcAddr[k], ip6->DstAddr[k]);
                            ip6->Length = htons(static_cast<u_short>(sizeof(WINDIVERT_UDPHDR) + resp.size()));
                            newLen = static_cast<UINT>(sizeof(WINDIVERT_IPV6HDR) + sizeof(WINDIVERT_UDPHDR) + resp.size());
                        }
                        std::swap(udph->SrcPort, udph->DstPort);
                        udph->Length = htons(static_cast<u_short>(sizeof(WINDIVERT_UDPHDR) + resp.size()));
                        std::memcpy(payload, resp.data(), resp.size());
                        addr.Outbound = addr.Outbound ? 0 : 1;
                        addr.Impostor = 1;
                        WinDivertHelperCalcChecksums(packet.data(), newLen, &addr, 0);
                        UINT sent = 0;
                        WinDivertSend(h, packet.data(), newLen, &sent, &addr);
                        log_line(c, "DNS " + name + " -> 127.0.0.1");
                        handled = true;
                    }
                }
            }
        }
        if (!handled) {
            UINT sent = 0;
            WinDivertSend(h, packet.data(), recvLen, &sent, &addr);
        }
    }
    log_line(c, "WinDivert DNS interceptor stopping");
}

static void tcp_redirect_thread(const Config& c, std::atomic<bool>& stop) {
    std::ostringstream filter;
    filter << "loopback and ((tcp.DstPort == 443 or tcp.DstPort == 22 or tcp.DstPort == 9418 or tcp.DstPort == 80) or "
           << "(tcp.SrcPort == " << c.redirect_https_port << " or tcp.SrcPort == " << c.redirect_ssh_port
           << " or tcp.SrcPort == " << c.redirect_git_port << " or tcp.SrcPort == " << c.redirect_http_port << "))";
    HANDLE h = WinDivertOpen(filter.str().c_str(), WINDIVERT_LAYER_NETWORK, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        log_line(c, "WinDivert TCP handle failed (need administrator). WinDivert error=" + std::to_string(GetLastError()));
        return;
    }
    g_tcp_divert.store(h);
    log_line(c, "WinDivert TCP redirector started");
    std::vector<uint8_t> packet(65535);
    while (!stop.load()) {
        UINT recvLen = 0;
        WINDIVERT_ADDRESS addr{};
        if (!WinDivertRecv(h, packet.data(), static_cast<UINT>(packet.size()), &recvLen, &addr)) continue;
        WINDIVERT_TCPHDR* tcp = nullptr;
        if (WinDivertHelperParsePacket(packet.data(), recvLen, nullptr, nullptr, nullptr, nullptr, nullptr,
                                       &tcp, nullptr, nullptr, nullptr, nullptr, nullptr) && tcp) {
            uint16_t d = ntohs(tcp->DstPort);
            uint16_t s = ntohs(tcp->SrcPort);
            bool changed = false;
            if (d == 443) { tcp->DstPort = htons(c.redirect_https_port); changed = true; }
            else if (d == 22) { tcp->DstPort = htons(c.redirect_ssh_port); changed = true; }
            else if (d == 9418) { tcp->DstPort = htons(c.redirect_git_port); changed = true; }
            else if (d == 80) { tcp->DstPort = htons(c.redirect_http_port); changed = true; }
            else if (s == c.redirect_https_port) { tcp->SrcPort = htons(443); changed = true; }
            else if (s == c.redirect_ssh_port) { tcp->SrcPort = htons(22); changed = true; }
            else if (s == c.redirect_git_port) { tcp->SrcPort = htons(9418); changed = true; }
            else if (s == c.redirect_http_port) { tcp->SrcPort = htons(80); changed = true; }
            if (changed) {
                addr.Impostor = 1;
                WinDivertHelperCalcChecksums(packet.data(), recvLen, &addr, 0);
            }
        }
        UINT sent = 0;
        WinDivertSend(h, packet.data(), recvLen, &sent, &addr);
    }
    log_line(c, "WinDivert TCP redirector stopping");
}

enum class RedirectKind { Https, Http, Raw };

static void redirect_connection(SOCKET client, RedirectKind kind, const Config& c) {
    std::string host;
    std::vector<uint8_t> prefix;
    uint16_t upstreamPort = 443;
    set_timeout(client, 5000);
    if (kind == RedirectKind::Https) {
        char buf[8192];
        while (prefix.size() < 131072) {
            int n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            prefix.insert(prefix.end(), buf, buf + n);
            if (extract_sni(prefix, host)) break;
        }
        if (host.empty()) host = "github.com";
        upstreamPort = 443;
    } else if (kind == RedirectKind::Http) {
        std::string s;
        char buf[8192];
        while (s.size() < 65536 && s.find("\r\n\r\n") == std::string::npos) {
            int n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            s.append(buf, n);
        }
        prefix.assign(s.begin(), s.end());
        if (!extract_http_host(s, host)) host = "github.com";
        upstreamPort = 80;
    } else {
        host = "github.com";
        upstreamPort = 22;
    }
    set_timeout(client, 0);
    std::string prefix_str(prefix.begin(), prefix.end());
    Upstream up = connect_upstream_validated(host, upstreamPort, prefix_str, c);
    if (up.sock == INVALID_SOCKET) {
        closesocket(client); return;
    }
    if (!up.initial.empty())
        ::send(client, up.initial.data(), static_cast<int>(up.initial.size()), 0);
    relay(client, up.sock, c);
    closesocket(up.sock);
    closesocket(client);
}

static void redirect_listener(uint16_t port, RedirectKind kind, const Config& c, std::atomic<bool>& stop) {
    SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return;
    int yes = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        log_line(c, "bind redirect port " + std::to_string(port) + " failed");
        closesocket(l); return;
    }
    ::listen(l, 64);
    log_line(c, "redirect listener on 127.0.0.1:" + std::to_string(port));
    while (!stop.load()) {
        fd_set r; FD_ZERO(&r); FD_SET(l, &r);
        timeval tv{0, 500000};
        int n = ::select(0, &r, nullptr, nullptr, &tv);
        if (n <= 0) continue;
        SOCKET client = ::accept(l, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread([client, kind, c] { redirect_connection(client, kind, c); }).detach();
    }
    closesocket(l);
}

// ---------- original CONNECT proxy (kept for manual mode) ----------

static std::string read_request(SOCKET s) {
    set_timeout(s, 5000);
    std::string data;
    char buf[4096];
    while (data.size() < 16384) {
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, n);
        if (data.find("\r\n\r\n") != std::string::npos) break;
    }
    set_timeout(s, 0);
    return data;
}

static void handle_client(SOCKET client, const Config& c) {
    std::string req = read_request(client);
    if (req.empty()) { closesocket(client); return; }
    size_t line_end = req.find("\r\n");
    if (line_end == std::string::npos) { closesocket(client); return; }
    std::istringstream iss(req.substr(0, line_end));
    std::string method, target, version;
    iss >> method >> target >> version;
    std::string host;
    uint16_t port = 80;
    bool absolute = target.rfind("http://", 0) == 0;
    if (method == "CONNECT") {
        if (!parse_host_port(target, host, port)) {
            send_text(client, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            closesocket(client); return;
        }
    } else if (absolute) {
        std::string rest = target.substr(7);
        size_t slash = rest.find('/');
        std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
        if (!parse_host_port(hostport, host, port)) {
            send_text(client, "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n");
            closesocket(client); return;
        }
    } else {
        send_text(client, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n");
        closesocket(client); return;
    }
    log_line(c, method + " " + target);
    if (method == "CONNECT") {
        send_text(client, "HTTP/1.1 200 Connection Established\r\n\r\n");
        std::string prefix;
        set_timeout(client, 1000);
        char buf[8192];
        int n = ::recv(client, buf, sizeof(buf), 0);
        if (n > 0) prefix.assign(buf, n);
        set_timeout(client, 0);
        Upstream up = connect_upstream_validated(host, port, prefix, c);
        if (up.sock == INVALID_SOCKET) { closesocket(client); return; }
        if (!up.initial.empty())
            ::send(client, up.initial.data(), static_cast<int>(up.initial.size()), 0);
        relay(client, up.sock, c);
        closesocket(up.sock);
        closesocket(client);
        return;
    }

    Upstream up = connect_upstream(host, port, c);
    if (up.sock == INVALID_SOCKET) {
        send_text(client, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
        closesocket(client); return;
    }
    std::string rest = target.substr(7);
    size_t slash = rest.find('/');
    std::string origin = (slash == std::string::npos) ? "/" : rest.substr(slash);
    std::string rewritten = method + " " + origin + " " + version + "\r\n" + req.substr(line_end + 2);
    ::send(up.sock, rewritten.data(), static_cast<int>(rewritten.size()), 0);
    relay(client, up.sock, c);
    closesocket(up.sock);
    closesocket(client);
}

static void proxy_listener(const Config& c, std::atomic<bool>& stop) {
    SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return;
    int yes = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(c.port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        log_line(c, "bind proxy port " + std::to_string(c.port) + " failed");
        closesocket(l); return;
    }
    ::listen(l, 64);
    log_line(c, "CONNECT proxy on 127.0.0.1:" + std::to_string(c.port));
    while (!stop.load()) {
        fd_set r; FD_ZERO(&r); FD_SET(l, &r);
        timeval tv{0, 500000};
        int n = ::select(0, &r, nullptr, nullptr, &tv);
        if (n <= 0) continue;
        SOCKET client = ::accept(l, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread([client, c] { handle_client(client, c); }).detach();
    }
    closesocket(l);
}

// ---------- engine ----------

static void stats_writer(const Config& c, std::atomic<bool>& stop) {
    (void)c;
    uint64_t last_rx = 0, last_tx = 0;
    bool first = true;
    while (!stop.load()) {
        uint64_t rx = g_bytes_rx.load();
        uint64_t tx = g_bytes_tx.load();
        if (first || rx != last_rx || tx != last_tx) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (g_stats_view) {
                auto* sh = static_cast<StatsShared*>(g_stats_view);
                InterlockedExchange64(&sh->timestamp_ms, static_cast<LONG64>(ms));
                InterlockedExchange64(&sh->rx, static_cast<LONG64>(rx));
                InterlockedExchange64(&sh->tx, static_cast<LONG64>(tx));
            }
            last_rx = rx;
            last_tx = tx;
            first = false;
        }
        Sleep(500);
    }
}

static void run_engine(const Config& c, std::atomic<bool>& stop) {
    DnsFlushResolverCache();
    g_bytes_rx.store(0);
    g_bytes_tx.store(0);
    g_stats_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                     0, sizeof(StatsShared), STATS_MAP_NAME);
    if (g_stats_map)
        g_stats_view = MapViewOfFile(g_stats_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StatsShared));
    log_line(c, "engine started");
    std::vector<std::thread> threads;
    threads.emplace_back([&] { stats_writer(c, stop); });
    threads.emplace_back([&] { dns_intercept_thread(c, stop); });
    threads.emplace_back([&] { tcp_redirect_thread(c, stop); });
    threads.emplace_back([&] { redirect_listener(c.redirect_https_port, RedirectKind::Https, c, stop); });
    threads.emplace_back([&] { redirect_listener(c.redirect_http_port, RedirectKind::Http, c, stop); });
    threads.emplace_back([&] { redirect_listener(c.redirect_ssh_port, RedirectKind::Raw, c, stop); });
    threads.emplace_back([&] { redirect_listener(c.redirect_git_port, RedirectKind::Raw, c, stop); });
    threads.emplace_back([&] { proxy_listener(c, stop); });
    while (!stop.load()) Sleep(200);
    HANDLE hd = g_dns_divert.exchange(INVALID_HANDLE_VALUE);
    if (hd != INVALID_HANDLE_VALUE) WinDivertClose(hd);
    HANDLE ht = g_tcp_divert.exchange(INVALID_HANDLE_VALUE);
    if (ht != INVALID_HANDLE_VALUE) WinDivertClose(ht);
    for (auto& t : threads) if (t.joinable()) t.join();
    if (g_stats_view) { UnmapViewOfFile(g_stats_view); g_stats_view = nullptr; }
    if (g_stats_map) { CloseHandle(g_stats_map); g_stats_map = nullptr; }
    DnsFlushResolverCache();
    log_line(c, "engine stopped");
}

// ---------- service ----------

static const char* SERVICE_NAME = "FastGithubCpp";
static SERVICE_STATUS g_svc_status{};
static SERVICE_STATUS_HANDLE g_svc_handle = nullptr;

static void WINAPI svc_handler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        g_engine_stop.store(true);
        g_svc_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_svc_status.dwWaitHint = 5000;
        SetServiceStatus(g_svc_handle, &g_svc_status);
    }
}

static void WINAPI svc_main(DWORD, LPSTR*) {
    g_svc_handle = RegisterServiceCtrlHandlerA(SERVICE_NAME, svc_handler);
    if (!g_svc_handle) return;
    g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState = SERVICE_START_PENDING;
    g_svc_status.dwControlsAccepted = 0;
    SetServiceStatus(g_svc_handle, &g_svc_status);

    Config c;
    c.domains = split(default_domains(), ';');
    c.verbose = true;
    load_config_file(c);

    g_svc_status.dwCurrentState = SERVICE_RUNNING;
    g_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_svc_handle, &g_svc_status);

    run_engine(c, g_engine_stop);

    g_svc_status.dwCurrentState = SERVICE_STOPPED;
    g_svc_status.dwControlsAccepted = 0;
    SetServiceStatus(g_svc_handle, &g_svc_status);
}

static bool is_admin() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev{};
    DWORD len = sizeof(elev);
    BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &len);
    CloseHandle(tok);
    return ok && elev.TokenIsElevated;
}

static int service_start() {
    if (!is_admin()) { std::cerr << "Please run as administrator.\n"; return 1; }
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string bin = std::string("\"") + path + "\" --service";

    SC_HANDLE mgr = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!mgr) { std::cerr << "OpenSCManager failed: " << GetLastError() << "\n"; return 1; }
    SC_HANDLE svc = OpenServiceA(mgr, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!svc) {
        svc = CreateServiceA(mgr, SERVICE_NAME, "FastGithub C++ Accelerator", SERVICE_ALL_ACCESS,
                             SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) { std::cerr << "CreateService failed: " << GetLastError() << "\n"; CloseServiceHandle(mgr); return 1; }
        std::cout << "Service installed.\n";
    } else {
        std::cout << "Service already installed.\n";
        if (!ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                                  bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            std::cerr << "ChangeServiceConfig failed: " << GetLastError() << "\n";
        } else {
            std::cout << "Service path updated.\n";
        }
    }
    if (!StartServiceA(svc, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            std::cerr << "StartService failed: " << err << "\n";
            CloseServiceHandle(svc); CloseServiceHandle(mgr); return 1;
        }
        std::cout << "Service already running.\n";
    } else {
        std::cout << "Service started.\n";
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(mgr);
    return 0;
}

static int service_stop() {
    if (!is_admin()) { std::cerr << "Please run as administrator.\n"; return 1; }
    SC_HANDLE mgr = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!mgr) { std::cerr << "OpenSCManager failed: " << GetLastError() << "\n"; return 1; }
    SC_HANDLE svc = OpenServiceA(mgr, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!svc) { std::cerr << "Service not found.\n"; CloseServiceHandle(mgr); return 1; }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 40 && st.dwCurrentState != SERVICE_STOPPED; ++i) {
        Sleep(250);
        QueryServiceStatus(svc, &st);
    }
    if (!DeleteService(svc)) std::cerr << "DeleteService failed: " << GetLastError() << "\n";
    else std::cout << "Service stopped and removed.\n";
    CloseServiceHandle(svc);
    CloseServiceHandle(mgr);
    return 0;
}

static void selftest(const std::string& host, const Config& c) {
    std::cout << "Selftest host: " << host << "\n";
    auto ips = resolve_dns(host, c);
    std::cout << "DNS results (" << ips.size() << "):\n";
    for (auto& ip : ips) std::cout << "  " << ip << "\n";
    auto ranked = rank_ips(host, 443, ips, c);
    std::cout << "Reachable IPs (" << ranked.size() << "):\n";
    for (auto& ip : ranked) std::cout << "  " << ip << "\n";
    auto up = connect_upstream(host, 443, c);
    std::cout << "Final: " << (up.sock == INVALID_SOCKET ? "FAILED" : ("connected via " + up.ip)) << "\n";
    if (up.sock != INVALID_SOCKET) closesocket(up.sock);
}

int main(int argc, char** argv) {
    Config c;
    c.domains = split(default_domains(), ';');
    load_config_file(c);

    bool engine_mode = false;
    bool test_mode = false;
    std::string test_host = "github.com";
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "start") return service_start();
        if (a == "stop") return service_stop();
        if (a == "--service") {
            WsaInit wsa;
            SERVICE_TABLE_ENTRYA table[] = {{const_cast<char*>(SERVICE_NAME), svc_main}, {nullptr, nullptr}};
            StartServiceCtrlDispatcherA(table);
            return 0;
        }
        if (a == "--engine") engine_mode = true;
        else if (a == "--verbose" || a == "-v") c.verbose = true;
        else if (a == "--no-ipv6") c.ipv6 = false;
        else if (a == "--port" && i + 1 < argc) c.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (a == "--dns" && i + 1 < argc) c.dns_servers = split(argv[++i], ';');
        else if (a == "--domains" && i + 1 < argc) c.domains = split(argv[++i], ';');
        else if (a == "--selftest") { test_mode = true; if (i + 1 < argc && argv[i + 1][0] != '-') test_host = argv[++i]; }
        else if (a == "--help" || a == "-h") {
            std::cout << "FastGithubCpp\n"
                      << "  (no args)        run engine in console\n"
                      << "  --engine         run engine silently\n"
                      << "  start            install+start Windows service (admin)\n"
                      << "  stop             stop+remove Windows service (admin)\n"
                      << "  --selftest host  DNS/IP self test\n"
                      << "  -v               verbose log\n";
            return 0;
        }
    }

    WsaInit wsa;
    if (test_mode) { selftest(test_host, c); return 0; }

    if (!is_admin()) {
        std::cerr << "WARNING: not running as administrator; WinDivert transparent mode will be disabled.\n";
        std::cerr << "The manual CONNECT proxy on 127.0.0.1:" << c.port << " still works.\n";
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    if (!engine_mode) {
        std::cout << "FastGithubCpp starting. Transparent mode (no proxy setting needed) requires administrator.\n";
        std::cout << "Log file: " << exe_dir() << "\\fastgithubcpp.log\n";
        std::cout << "Press Ctrl+C to stop.\n";
    }

    run_engine(c, g_engine_stop);
    return 0;
}
