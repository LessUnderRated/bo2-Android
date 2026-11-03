// dw_local.cpp — local Demonware responder for the Zombies and multiplayer executables.
//
// Answers what the game's own client asks; winsock as documented by Microsoft.
//
// Shape. The game's own Demonware client runs (lifted); it talks winsock. This file IS winsock for it: sockets are
// table entries, nothing leaves the machine. Names under demonware.net resolve to private addresses; a TCP connect
// to one of them reaches the responder below, which answers what the client asks. Any other name does not resolve
// (WSAHOST_NOT_FOUND), as on a machine with only the Demonware hosts reachable.
//
// Guest addresses (VAs) used by the responder live in ONE table, kVa, by role: they must be re-taken on another
// build of the exe.

#include "win32_shim.h"
#include "bo2_getenv.h"

#include <CommonCrypto/CommonCryptor.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

extern "C" uint32_t win32_shim_alloc(uint32_t size, uint32_t align);

namespace {

// ---- winsock constants (public Microsoft documentation) ----
constexpr uint32_t SOCKET_ERROR_ = 0xFFFFFFFFu;
constexpr uint32_t WSAEWOULDBLOCK_ = 10035, WSAENOTSOCK_ = 10038, WSAECONNREFUSED_ = 10061,
                   WSAHOST_NOT_FOUND_ = 11001, WSAENOTCONN_ = 10057;
constexpr int SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2;

std::mutex g_mu;
thread_local uint32_t g_last_error = 0;

void put_bytes(uint32_t va, const uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) win32_mem_write8(va + i, p[i]); }
void get_bytes(uint32_t va, uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) p[i] = win32_mem_read8(va + i); }
std::string get_cstr(uint32_t va, uint32_t max = 256) {
    std::string s;
    for (uint32_t i = 0; i < max; i++) { char c = (char)win32_mem_read8(va + i); if (!c) break; s += c; }
    return s;
}

bool g_trace = bo2_env_on("BO2_DW_TRACE", true);

void hexdump(const char* tag, uint32_t sock, const uint8_t* p, size_t n) {
    if (!g_trace) return;
    size_t m = n < 512 ? n : 512;
    std::string h;
    for (size_t i = 0; i < m; i++) { char b[4]; snprintf(b, sizeof b, "%02x", p[i]); h += b; }
    fprintf(stderr, "[DWP] %s s=%u n=%zu %s%s\n", tag, sock, n, h.c_str(), m < n ? "..." : "");
}

// ---- the fake network ----
// 10.77.0.N: one address per Demonware name, in resolution order. The local host is 10.77.1.2.
constexpr uint32_t NET = 0x0A4D0000u, LOCAL_IP = 0x0A4D0102u;

struct Sock {
    int type = SOCK_STREAM_;
    bool connected = false;
    uint32_t peer_ip = 0; uint16_t peer_port = 0;            // host byte order
    std::deque<uint8_t> rx;                                  // TCP: bytes the responder sent
    std::deque<std::pair<std::vector<uint8_t>, std::pair<uint32_t, uint16_t>>> dgrams;   // UDP: payload, from
    std::vector<uint8_t> in;                                 // TCP: bytes the client sent, not yet consumed
    int lobby_stage = 0;                                     // lobby: 0 raw preamble due, 1 hello due, 2 tasks
};
std::map<uint32_t, Sock> g_socks;
uint32_t g_next_sock = 0x200;
std::map<std::string, uint32_t> g_name_ip;                   // name -> ip (host order)
std::map<std::string, uint32_t> g_hostent;                   // name -> guest hostent

std::string name_of(uint32_t ip) {
    for (auto& kv : g_name_ip) if (kv.second == ip) return kv.first;
    return "?";
}

void server_on_data(uint32_t s, Sock& k);                   // the responder (below)

// fd_set (winsock): u_int fd_count; SOCKET fd_array[64].
std::vector<uint32_t> fdset_read(uint32_t va) {
    std::vector<uint32_t> v;
    if (!va) return v;
    uint32_t n = win32_mem_read32(va);
    for (uint32_t i = 0; i < n && i < 64; i++) v.push_back(win32_mem_read32(va + 4 + 4 * i));
    return v;
}
void fdset_write(uint32_t va, const std::vector<uint32_t>& v) {
    if (!va) return;
    win32_mem_write32(va, (uint32_t)v.size());
    for (size_t i = 0; i < v.size(); i++) win32_mem_write32(va + 4 + 4 * (uint32_t)i, v[i]);
}

uint32_t fail(uint32_t err) { g_last_error = err; return SOCKET_ERROR_; }

}  // namespace

// ---- winsock entry points (ordinals as in ws2_32 / wsock32) ----

// socket(af, type, protocol)
void impl_wsock_ordinal_23(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t s = g_next_sock++;
    g_socks[s].type = (int)win32_arg(st, 1);
    if (g_trace) fprintf(stderr, "[DWP] socket type=%d -> %u\n", g_socks[s].type, s);
    st.gpr.rax.dword = s;
    win32_stdcall_ret(st, 3);
}

// closesocket(s)
void impl_wsock_ordinal_3(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t s = win32_arg(st, 0);
    st.gpr.rax.dword = g_socks.erase(s) ? 0 : fail(WSAENOTSOCK_);
    if (g_trace) fprintf(stderr, "[DWP] closesocket %u\n", s);
    win32_stdcall_ret(st, 1);
}

// bind(s, name, namelen): any local address is accepted.
void impl_wsock_ordinal_2(State& st) { st.gpr.rax.dword = 0; win32_stdcall_ret(st, 3); }

// connect(s, name, namelen)
void impl_wsock_ordinal_4(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t s = win32_arg(st, 0), sa = win32_arg(st, 1);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) { st.gpr.rax.dword = fail(WSAENOTSOCK_); win32_stdcall_ret(st, 3); return; }
    uint16_t port = (uint16_t)(win32_mem_read8(sa + 2) << 8 | win32_mem_read8(sa + 3));
    uint32_t ip = (uint32_t)win32_mem_read8(sa + 4) << 24 | win32_mem_read8(sa + 5) << 16 |
                  win32_mem_read8(sa + 6) << 8 | win32_mem_read8(sa + 7);
    bool ours = (ip & 0xFFFF0000u) == NET && ip != LOCAL_IP;
    if (g_trace) fprintf(stderr, "[DWP] connect %u -> %s (%u.%u.%u.%u:%u) %s\n", s, name_of(ip).c_str(), ip >> 24,
                         ip >> 16 & 255, ip >> 8 & 255, ip & 255, port, ours ? "ok" : "refuse");
    if (!ours) { st.gpr.rax.dword = fail(WSAECONNREFUSED_); win32_stdcall_ret(st, 3); return; }
    it->second.peer_ip = ip; it->second.peer_port = port; it->second.connected = true;
    st.gpr.rax.dword = 0;
    win32_stdcall_ret(st, 3);
}

// send(s, buf, len, flags)
void impl_wsock_ordinal_19(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t s = win32_arg(st, 0), buf = win32_arg(st, 1), len = win32_arg(st, 2);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) { st.gpr.rax.dword = fail(WSAENOTSOCK_); win32_stdcall_ret(st, 4); return; }
    if (!it->second.connected) { st.gpr.rax.dword = fail(WSAENOTCONN_); win32_stdcall_ret(st, 4); return; }
    std::vector<uint8_t> d(len);
    get_bytes(buf, d.data(), len);
    hexdump("send", s, d.data(), len);
    it->second.in.insert(it->second.in.end(), d.begin(), d.end());
    server_on_data(s, it->second);
    st.gpr.rax.dword = len;
    win32_stdcall_ret(st, 4);
}

void readiness_witness();
// recv(s, buf, len, flags)
void impl_wsock_ordinal_16(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    readiness_witness();
    uint32_t s = win32_arg(st, 0), buf = win32_arg(st, 1), len = win32_arg(st, 2);
    auto it = g_socks.find(s);
    if (it == g_socks.end()) { st.gpr.rax.dword = fail(WSAENOTSOCK_); win32_stdcall_ret(st, 4); return; }
    auto& rx = it->second.rx;
    if (rx.empty()) { st.gpr.rax.dword = fail(WSAEWOULDBLOCK_); win32_stdcall_ret(st, 4); return; }
    uint32_t n = len < rx.size() ? len : (uint32_t)rx.size();
    std::vector<uint8_t> d(rx.begin(), rx.begin() + n);
    rx.erase(rx.begin(), rx.begin() + n);
    put_bytes(buf, d.data(), n);
    hexdump("recv", s, d.data(), n);
    st.gpr.rax.dword = n;
    win32_stdcall_ret(st, 4);
}

// STUN, client side. Address discovery (older t6zm build: 0xa522d0 sends, 0xa52460 reads): request [0x1e][u16 version 2]; reply
// [type 0x1e..0x27][u16 2][in_addr 4][u16 port] (0xa590e0, 0xa2a890), the address the server saw. NAT type discovery
// (0xa52ba0, handler 0xa527f0): request [0x14][u16 2][u8 sub]; reply [0x15][u16 2][bdAddr seen +4][bdAddr other
// server +0xc] (0xa59370). The first reply (state 1) stores both and sends sub 3; a reply then arriving from the
// other server's IP on another port gives state 4, type 1 (open). Without any reply the test fails (state 5) and
// bdNetStart (0xa4871b needs states 2 and 4) restarts, so the menu never counts as online.
constexpr uint8_t STUN_ADDR_REQ = 0x1e, STUN_ADDR_REPLY = 0x1f, STUN_NAT_REQ = 0x14, STUN_NAT_REPLY = 0x15;
void put_addr(std::vector<uint8_t>& r, uint32_t ip, uint16_t port) {
    r.insert(r.end(), {(uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip,
                       (uint8_t)port, (uint8_t)(port >> 8)});
}
void stun(uint32_t s, const uint8_t* d, uint32_t len, uint32_t ip, uint16_t port) {
    if (len < 3 || name_of(ip).find("-stun.") == std::string::npos) return;
    std::vector<uint8_t> r;
    if (d[0] == STUN_ADDR_REQ) {
        r = {STUN_ADDR_REPLY, 2, 0};
        put_addr(r, LOCAL_IP, 3074);
    } else if (d[0] == STUN_NAT_REQ) {
        r = {STUN_NAT_REPLY, 2, 0};
        put_addr(r, LOCAL_IP, 3074);
        put_addr(r, ip, (uint16_t)(port + 1));   // "other server": same IP, so every reply comes from another port
    } else {
        return;
    }
    g_socks[s].dgrams.push_back({r, {ip, port}});
}

// sendto(s, buf, len, flags, to, tolen): only the STUN requests get an answer.
void impl_wsock_ordinal_20(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t s = win32_arg(st, 0), buf = win32_arg(st, 1), len = win32_arg(st, 2), to = win32_arg(st, 4);
    if (to && len < 64) {
        uint8_t d[64];
        get_bytes(buf, d, len);
        stun(s, d, len,
             (uint32_t)win32_mem_read8(to + 4) << 24 | win32_mem_read8(to + 5) << 16 | win32_mem_read8(to + 6) << 8 |
                 win32_mem_read8(to + 7),
             (uint16_t)(win32_mem_read8(to + 2) << 8 | win32_mem_read8(to + 3)));
    }
    if (g_trace && to) {
        std::vector<uint8_t> d(len < 64 ? len : 64);
        get_bytes(buf, d.data(), (uint32_t)d.size());
        uint32_t ip = (uint32_t)win32_mem_read8(to + 4) << 24 | win32_mem_read8(to + 5) << 16 |
                      win32_mem_read8(to + 6) << 8 | win32_mem_read8(to + 7);
        char tag[96];
        snprintf(tag, sizeof tag, "sendto %s:%u", name_of(ip).c_str(), win32_mem_read8(to + 2) << 8 | win32_mem_read8(to + 3));
        hexdump(tag, s, d.data(), d.size());
    }
    st.gpr.rax.dword = len;
    win32_stdcall_ret(st, 6);
}

// recvfrom(s, buf, len, flags, from, fromlen)
void impl_wsock_ordinal_17(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t s = win32_arg(st, 0), buf = win32_arg(st, 1), len = win32_arg(st, 2), from = win32_arg(st, 4),
             fromlen = win32_arg(st, 5);
    auto it = g_socks.find(s);
    if (it == g_socks.end() || it->second.dgrams.empty()) {
        st.gpr.rax.dword = fail(it == g_socks.end() ? WSAENOTSOCK_ : WSAEWOULDBLOCK_);
        win32_stdcall_ret(st, 6);
        return;
    }
    auto dg = it->second.dgrams.front();
    it->second.dgrams.pop_front();
    uint32_t n = len < dg.first.size() ? len : (uint32_t)dg.first.size();
    put_bytes(buf, dg.first.data(), n);
    if (from) {
        uint8_t sa[16] = {2, 0, (uint8_t)(dg.second.second >> 8), (uint8_t)dg.second.second,
                          (uint8_t)(dg.second.first >> 24), (uint8_t)(dg.second.first >> 16),
                          (uint8_t)(dg.second.first >> 8), (uint8_t)dg.second.first};
        put_bytes(from, sa, 16);
        if (fromlen) win32_mem_write32(fromlen, 16);
    }
    st.gpr.rax.dword = n;
    win32_stdcall_ret(st, 6);
}

// select(nfds, readfds, writefds, exceptfds, timeout): never waits, answers the state now.
void impl_wsock_ordinal_18(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t rd = win32_arg(st, 1), wr = win32_arg(st, 2), ex = win32_arg(st, 3);
    std::vector<uint32_t> r, w;
    for (uint32_t s : fdset_read(rd)) {
        auto it = g_socks.find(s);
        if (it != g_socks.end() && (!it->second.rx.empty() || !it->second.dgrams.empty())) r.push_back(s);
    }
    for (uint32_t s : fdset_read(wr)) {
        auto it = g_socks.find(s);
        if (it != g_socks.end() && (it->second.connected || it->second.type == SOCK_DGRAM_)) w.push_back(s);
    }
    fdset_write(rd, r); fdset_write(wr, w); fdset_write(ex, {});
    st.gpr.rax.dword = (uint32_t)(r.size() + w.size());
    win32_stdcall_ret(st, 5);
}

// __WSAFDIsSet(s, set)
void impl_wsock_ordinal_151(State& st) {
    uint32_t s = win32_arg(st, 0);
    uint32_t r = 0;
    for (uint32_t x : fdset_read(win32_arg(st, 1))) if (x == s) r = 1;
    st.gpr.rax.dword = r;
    win32_stdcall_ret(st, 2);
}

// inet_addr(cp) (wsock32 numbering: 10 = inet_addr, 12 = ioctlsocket): dotted quad -> address in network order,
// INADDR_NONE if it does not parse. The UPnP client parses "239.255.255.250" with it every frame.
void impl_wsock_ordinal_10(State& st) {
    st.gpr.rax.dword = inet_addr(get_cstr(win32_arg(st, 0)).c_str());
    win32_stdcall_ret(st, 1);
}

// htons(hostshort)
void impl_wsock_ordinal_9(State& st) {
    st.gpr.rax.dword = __builtin_bswap16((uint16_t)win32_arg(st, 0));
    win32_stdcall_ret(st, 1);
}

// gethostname(name, namelen)
void impl_wsock_ordinal_57(State& st) {
    uint32_t name = win32_arg(st, 0), len = win32_arg(st, 1);
    static const char kHost[] = "bo2-native";
    if (len < sizeof kHost) { st.gpr.rax.dword = fail(10014 /* WSAEFAULT */); win32_stdcall_ret(st, 2); return; }
    put_bytes(name, (const uint8_t*)kHost, sizeof kHost);
    st.gpr.rax.dword = 0;
    win32_stdcall_ret(st, 2);
}

// gethostbyname(name): demonware.net names and this host resolve; anything else does not.
void impl_wsock_ordinal_52(State& st) {
    std::lock_guard<std::mutex> l(g_mu);
    std::string n = get_cstr(win32_arg(st, 0));
    auto ends = [&](const char* suf) { size_t k = strlen(suf); return n.size() >= k && n.compare(n.size() - k, k, suf) == 0; };
    uint32_t ip = 0;
    if (n == "bo2-native" || n == "localhost") ip = LOCAL_IP;
    else if (ends(".demonware.net")) {
        auto f = g_name_ip.find(n);
        ip = f != g_name_ip.end() ? f->second : NET + 1 + (uint32_t)g_name_ip.size();
    }
    if (g_trace) fprintf(stderr, "[DWP] gethostbyname \"%s\" -> %s\n", n.c_str(), ip ? "ok" : "absent");
    if (!ip) { g_last_error = WSAHOST_NOT_FOUND_; st.gpr.rax.dword = 0; win32_stdcall_ret(st, 1); return; }
    g_name_ip[n] = ip;
    uint32_t& he = g_hostent[n];
    if (!he) {
        // hostent { char* h_name; char** h_aliases; short h_addrtype; short h_length; char** h_addr_list; }
        // followed by: aliases {0}, addr_list {&addr, 0}, addr (4, network order), name.
        he = win32_shim_alloc(16 + 4 + 8 + 4 + (uint32_t)n.size() + 1, 16);
        uint32_t aliases = he + 16, list = he + 20, addr = he + 28, nm = he + 32;
        win32_mem_write32(he, nm);
        win32_mem_write32(he + 4, aliases);
        win32_mem_write32(he + 8, 2 | 4u << 16);             // AF_INET, 4 bytes
        win32_mem_write32(he + 12, list);
        win32_mem_write32(aliases, 0);
        win32_mem_write32(list, addr);
        win32_mem_write32(list + 4, 0);
        uint8_t a[4] = {(uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip};
        put_bytes(addr, a, 4);
        put_bytes(nm, (const uint8_t*)n.c_str(), (uint32_t)n.size() + 1);
    }
    st.gpr.rax.dword = he;
    win32_stdcall_ret(st, 1);
}

// WSAGetLastError()
void impl_wsock_ordinal_111(State& st) { st.gpr.rax.dword = g_last_error; win32_stdcall_ret(st, 0); }

// inet_ntoa(in): one argument, the address in network order.
void impl_ws2_ordinal_12(State& st) {
    static uint32_t buf = 0;
    if (!buf) buf = win32_shim_alloc(16, 16);
    uint32_t a = win32_arg(st, 0);
    char s[16];
    snprintf(s, sizeof s, "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24);
    put_bytes(buf, (const uint8_t*)s, (uint32_t)strlen(s) + 1);
    st.gpr.rax.dword = buf;
    win32_stdcall_ret(st, 1);
}

// ---- the responder ----
namespace {

// Guest addresses by role. Everything else is computed. Found in t6mp by the same code patterns (journal).
struct GuestVa {
    uint32_t tiger_table;        // LibTomCrypt Tiger S-boxes t1..t4, 4 x 256 x u64, contiguous
    uint32_t auth_service_slot;  // pointer to the bdAuthService singleton
};
#if defined(BO2_TARGET_ZMS)
constexpr GuestVa kVa = {0xc00290, 0x1734990};
#elif defined(BO2_TARGET_MPS)
constexpr GuestVa kVa = {0xc02680, 0x1736910};
#else
constexpr GuestVa kVa = {0, 0};  // not taken yet for this exe: the responder refuses (see [DWP] VA)
#endif
// Witness: the flags the menu's "online service ready" test reads for controller 0 (older t6zm build 0x45d180, required mask
// 0x411ee; older t6mp build 0x444020, mask 0x51bee = the same plus bits 0x200, 0x800, 0x10000). Printed by [DWP-PRET] each time one
// changes. Roles: dw_etat/dw_signe = Demonware status and sign-in (bit 2), stats (bit 8), etat_reseau == 6 (bit 0x20),
// ffotd_lu, wad_fini, ffotd_version >= wad_version (bit 0x100), playlists (bit 0x80), b* = the bit a byte stands for.
struct Flag { const char* role; uint32_t va; int bytes; };
#if defined(BO2_TARGET_ZMS)
// Same test at 0x83fde0, flags read from its callees (0x86c180, 0x86c1b0, 0x86b860, 0x864480); the older build's block + -0xafcc8.
constexpr Flag kReady[] = {
    {"stats", 0x2ebc97b, 1}, {"etat_reseau", 0xc880a4, 4}, {"b40", 0x2f1da24, 1}, {"playlists", 0x2f1da2a, 1},
    {"ffotd_lu", 0x2f1da38, 1}, {"wad_fini", 0x2f1da39, 1}, {"wad_version", 0x2f1da3c, 4},
    {"ffotd_version", 0xf66548, 4}, {"b1000", 0x2e28d79, 1}, {"ffotd_taille", 0x2f1da10, 4}, {"ffotd_parts", 0x2f1da18, 4},
    {"ffotd_charge", 0x2f1da33, 1}, {"ffotd_recu", 0x2f1dd58, 4},
};
#elif defined(BO2_TARGET_MPS)
// Same test at 0x83efb0 (mask 0x41bee), the older build's flags through the correspondence table.
constexpr Flag kReady[] = {
    {"stats", 0x2ebe8fb, 1}, {"etat_reseau", 0xc8a084, 4}, {"b40", 0x2f1f9a4, 1}, {"playlists", 0x2f1f9aa, 1},
    {"ffotd_lu", 0x2f1f9b8, 1}, {"wad_fini", 0x2f1f9b9, 1}, {"wad_version", 0x2f1f9bc, 4},
    {"ffotd_version", 0xf684c8, 4}, {"b1000", 0x2e2acf9, 1},
};
#else
constexpr Flag kReady[] = {{"aucun", 0, 0}};
#endif
void readiness_witness_impl() {
    static uint32_t last[sizeof kReady / sizeof *kReady];
    static bool first = true;
    std::string line;
    bool changed = first;
    for (size_t i = 0; i < sizeof kReady / sizeof *kReady; i++) {
        if (!kReady[i].va) return;
        uint32_t v = kReady[i].bytes == 4 ? win32_mem_read32(kReady[i].va) : win32_mem_read8(kReady[i].va);
        if (v != last[i]) changed = true;
        last[i] = v;
        char b[48];
        snprintf(b, sizeof b, " %s=%d", kReady[i].role, (int)v);
        line += b;
    }
    first = false;
    if (changed && g_trace) fprintf(stderr, "[DWP-PRET]%s\n", line.c_str());
}

// Offsets inside bdAuthService.
constexpr uint32_t AUTH_TICKET_KEY = 0x150;  // 24 bytes the client drew and put in the Steam ticket's user data
constexpr uint32_t AUTH_USER_NAME = 0x168;   // 64 bytes

// Message types on the Demonware connection (first byte after the frame header).
constexpr uint8_t MSG_AUTH_STEAM = 0x1c, MSG_AUTH_STEAM_REPLY = 0x1d;
constexpr uint32_t AUTH_OK = 0x2bc;
constexpr uint32_t TICKET_MAGIC = 0xefbdadde;

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
void put_le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }

// Tiger/192 (Anderson & Biham), tables read from the game. Checked against the published vector for "abc".
std::vector<uint64_t> g_tiger;
bool tiger(const uint8_t* m, size_t n, uint8_t out[24]);
bool tiger_ready() {
    if (!g_tiger.empty()) return true;
    if (!kVa.tiger_table) return false;
    std::vector<uint8_t> raw(8192);
    get_bytes(kVa.tiger_table, raw.data(), 8192);
    g_tiger.resize(1024);
    memcpy(g_tiger.data(), raw.data(), 8192);
    uint8_t h[24];
    static const uint8_t kAbc[8] = {0x2a, 0xab, 0x14, 0x84, 0xe8, 0xc1, 0x58, 0xf2};
    if (!tiger((const uint8_t*)"abc", 3, h) || memcmp(h, kAbc, 8)) {
        fprintf(stderr, "[DWP] VA: Tiger tables at 0x%x do not give the published vector, responder off\n", kVa.tiger_table);
        g_tiger.clear();
        return false;
    }
    return true;
}
bool tiger(const uint8_t* m, size_t n, uint8_t out[24]) {
    if (g_tiger.size() != 1024) return false;
    const uint64_t *t1 = &g_tiger[0], *t2 = &g_tiger[256], *t3 = &g_tiger[512], *t4 = &g_tiger[768];
    uint64_t a = 0x0123456789ABCDEFull, b = 0xFEDCBA9876543210ull, c = 0xF096A5B4C3B2E187ull;
    std::vector<uint8_t> p(m, m + n);
    p.push_back(0x01);
    while (p.size() % 64 != 56) p.push_back(0);
    for (int i = 0; i < 8; i++) p.push_back((uint8_t)((uint64_t)n * 8 >> 8 * i));
    auto round = [&](uint64_t& a, uint64_t& b, uint64_t& c, uint64_t x, uint64_t mul) {
        c ^= x;
        a -= t1[c & 255] ^ t2[c >> 16 & 255] ^ t3[c >> 32 & 255] ^ t4[c >> 48 & 255];
        b += t4[c >> 8 & 255] ^ t3[c >> 24 & 255] ^ t2[c >> 40 & 255] ^ t1[c >> 56];
        b *= mul;
    };
    auto pass = [&](uint64_t& a, uint64_t& b, uint64_t& c, uint64_t* x, uint64_t mul) {
        round(a, b, c, x[0], mul); round(b, c, a, x[1], mul); round(c, a, b, x[2], mul); round(a, b, c, x[3], mul);
        round(b, c, a, x[4], mul); round(c, a, b, x[5], mul); round(a, b, c, x[6], mul); round(b, c, a, x[7], mul);
    };
    auto schedule = [](uint64_t* x) {
        x[0] -= x[7] ^ 0xA5A5A5A5A5A5A5A5ull; x[1] ^= x[0]; x[2] += x[1]; x[3] -= x[2] ^ (~x[1] << 19);
        x[4] ^= x[3]; x[5] += x[4]; x[6] -= x[5] ^ (~x[4] >> 23); x[7] ^= x[6];
        x[0] += x[7]; x[1] -= x[0] ^ (~x[7] << 19); x[2] ^= x[1]; x[3] += x[2];
        x[4] -= x[3] ^ (~x[2] >> 23); x[5] ^= x[4]; x[6] += x[5]; x[7] -= x[6] ^ 0x0123456789ABCDEFull;
    };
    for (size_t off = 0; off < p.size(); off += 64) {
        uint64_t x[8];
        memcpy(x, &p[off], 64);
        uint64_t aa = a, bb = b, cc = c;
        pass(a, b, c, x, 5); schedule(x); pass(c, a, b, x, 7); schedule(x); pass(b, c, a, x, 9);
        a ^= aa; b -= bb; c += cc;
    }
    memcpy(out, &a, 8); memcpy(out + 8, &b, 8); memcpy(out + 16, &c, 8);
    return true;
}
// The 8-byte CBC vector of a message: Tiger of its counter (u32 little-endian), first 8 bytes.
bool iv_of(uint32_t counter, uint8_t iv[8]) {
    uint8_t c[4], h[24];
    put_le32(c, counter);
    if (!tiger(c, 4, h)) return false;
    memcpy(iv, h, 8);
    return true;
}
bool des3_cbc(bool enc, const uint8_t key[24], const uint8_t iv[8], std::vector<uint8_t>& buf) {
    std::vector<uint8_t> out(buf.size());
    size_t moved = 0;
    bool ok = CCCrypt(enc ? kCCEncrypt : kCCDecrypt, kCCAlgorithm3DES, 0, key, 24, iv, buf.data(), buf.size(),
                      out.data(), out.size(), &moved) == kCCSuccess && moved == buf.size();
    if (ok) buf.swap(out);
    return ok;
}

// The game's bit stream: fields one after the other, least significant bit first. A body starts with one bit that
// says whether each value carries a 5-bit type; the responder writes 0 (no types), which the client accepts.
struct Bits {
    std::vector<uint8_t> b;
    size_t n = 0;
    void put(uint64_t v, int bits) {
        for (int i = 0; i < bits; i++, n++) {
            if (n / 8 >= b.size()) b.push_back(0);
            if (v >> i & 1) b[n / 8] |= (uint8_t)(1 << n % 8);
        }
    }
    void raw(const uint8_t* p, size_t len) { for (size_t i = 0; i < len; i++) put(p[i], 8); }
};

uint8_t g_session_key[24];   // chosen here, given to the client inside the auth ticket, then keys the lobby channel
bool g_have_session_key = false;

// Frame to the client, in clear: [u32 n+1][flag 0][type][body]. The client takes clear frames on every connection.
void reply(Sock& k, uint8_t type, const std::vector<uint8_t>& body) {
    uint8_t h[6];
    put_le32(h, (uint32_t)body.size() + 2);
    h[4] = 0;
    h[5] = type;
    k.rx.insert(k.rx.end(), h, h + 6);
    k.rx.insert(k.rx.end(), body.begin(), body.end());
}

// Steam auth (request 0x1c): answer 0x1d = status, seed, ticket encrypted with the key the client drew, lobby ticket.
void auth_steam(uint32_t s, Sock& k) {
    uint32_t svc = kVa.auth_service_slot ? win32_mem_read32(kVa.auth_service_slot) : 0;
    if (!svc || !tiger_ready()) { fprintf(stderr, "[DWP] auth: VA absentes ou fausses, pas de réponse\n"); return; }
    uint8_t key[24];
    get_bytes(svc + AUTH_TICKET_KEY, key, 24);
    if (!g_have_session_key) { arc4random_buf(g_session_key, 24); g_have_session_key = true; }
    std::vector<uint8_t> t(128, 0);
    put_le32(&t[0], TICKET_MAGIC);
    get_bytes(svc + AUTH_USER_NAME, &t[33], 63);
    memcpy(&t[97], g_session_key, 24);
    const uint32_t seed = 1;
    uint8_t iv[8];
    if (!iv_of(seed, iv) || !des3_cbc(true, key, iv, t)) { fprintf(stderr, "[DWP] auth: chiffrement refusé\n"); return; }
    Bits w;
    w.put(0, 1);
    w.put(AUTH_OK, 32);
    w.put(seed, 32);
    w.raw(t.data(), 128);
    std::vector<uint8_t> lobby_ticket(128, 0);
    w.raw(lobby_ticket.data(), 128);
    reply(k, MSG_AUTH_STEAM_REPLY, w.b);
    if (g_trace) fprintf(stderr, "[DWP] auth s=%u: ticket envoyé (%zu octets), nom=\"%s\"\n", s, w.b.size(), get_cstr(svc + AUTH_USER_NAME, 64).c_str());
}

// Lobby connection, client side (0xa2e030, run on connect): 8 raw bytes [u32 0xc8][u32 0xffff], then one clear frame
// carrying the lobby ticket. The client counts as connected only once the lobby sends type 4 = typed u64 (0xa2fa5f,
// kept by the task manager); until then the menu shows "Connecting to Online Service".
constexpr size_t LOBBY_PREAMBLE = 8;
constexpr uint8_t MSG_LOBBY_CONNECTED = 4;
void lobby_hello(uint32_t s, Sock& k) {
    std::vector<uint8_t> b = {0xa};
    for (int i = 0; i < 8; i++) b.push_back(i == 0);   // u64 1
    reply(k, MSG_LOBBY_CONNECTED, b);
    if (g_trace) fprintf(stderr, "[DWP] salon s=%u: connecté (type 4)\n", s);
}

// Lobby task, client -> lobby: [service][typed u8 task][typed params][0]. Values in a lobby buffer carry a one-byte
// type before them (3 u8, 8 u32, 0xa u64). The answer is type 1, matched by the client to its oldest pending task:
// transaction u64 (ours to pick), error u32 (0 = done), u8, result count u32 (0 = no results).
// With results: count u32, total u32, then each result as its class reads it (0xa315a0). A file's content is one
// result: blob = [0x13][typed u32 length][bytes] (0xa3e210 → 0xa29380).
constexpr uint8_t MSG_LOBBY_TASK_REPLY = 1;
constexpr uint8_t SVC_FILES = 10, TASK_READ_PUBLISHER_FILE = 7, TASK_READ_USER_FILE = 3, TASK_WRITE_USER_FILE = 1;
constexpr uint8_t SVC_TITLE = 12, TASK_SERVER_TIME = 6, SVC_DML = 27, TASK_USER_DATA = 2;
// bdLeague: one solo team in preseason, nothing is ranked here. 0x691ed0 walks team record, subdivision, stats;
// empty stats get default values (0x68faf0).
constexpr uint8_t SVC_LEAGUE = 81, TASK_LEAGUE_TEAM_ID = 1, TASK_LEAGUE_SUBDIVISION = 3, TASK_LEAGUE_TEAM_INFO = 6;
constexpr uint64_t LEAGUE_TEAM_ID = 1, LEAGUE_RANKED_ID = 10, LEAGUE_SUBDIVISION_ID = 1;

// The online WAD (older t6zm build: 0x627890 asks for "online_tu15_zm_english.wad", 0x91fc30 reads it): big-endian header
// [magic 54 33 77 ab][u32][u32 entry count][u32 version], then entries of 0x2c bytes [name 0x20][u32 compressed
// size][u32 size][u32 offset], each a zlib stream (0x6fa680, window 15) inflated into a 0x40000 buffer. One entry,
// "playlists_zm.info" (multiplayer: "playlists_mp.info", "leagues.info"). Version 0xffffffff (-1) is not newer than the patch the game has (it reads -1 when no patch
// came, 0x91f930), as the menu requires.
//
// The playlist text, parsed line by line by 0x475770 (older t6zm build). Keywords open a block: "gametype" needs a "script" or
// the game drops ("Unknown gametype name"); "supercategory" and "category" are what the menus list
// (Engine.GetPlaylistSuperCategories / GetPlaylistCategories), and SOLO keeps those whose "filter" is "solomatch";
// a category joins its supercategory by naming it; "playlist <id>" names its category and lists entries
// "<map>,<gametype>,<start location>,<weight>": an entry line is re-read in comma mode (0x5145f0(1) → 0x8c9820),
// where a space belongs to the field, and a weight ≤ 0 drops it. Blocks are read in order: a name is known only once defined.
// "setdvar" is accepted only inside a "settings" block (0x47591f -> 0x476059) and applies as the file is read (0x5da310).
// Zombies: the structure of the shipped playlist, English only: the globe lists the supercategories of the owned maps,
// the mode list the playlists of a category, and a solo playlist carries "rule party_maxplayers 1" (P1043).
// "rule party_minplayers" is 1 everywhere so that a public match starts alone, as in multiplayer.
// Multiplayer: same format; "all,<gametype>,<weight>" takes every base map, and "categories" names the category.
// party_minplayers 1 lets a lone host start.
constexpr char kPlaylistsZm[] = R"PL(version 0627
settings
setdvar sv_writeconfigstrings 0
setdvar scr_writeconfigstrings 0
setdvar missingCommandWillError 0
setdvar cg_ScoresPing_Interval 1
setdvar demo_downloadStreamDataBlockThrottleTime 3000
setdvar ui_inGameStoreVisible 1
setdvar tu8_mtx_enabled 2
setdvar ui_showNewestLeaderboards 1
setdvar excellentPing 150
setdvar goodPing 300
setdvar terriblePing 350
gametype zclassic
name english "STORY"
rule ui_zm_gamemodegroup zclassic
script zclassic
gametype zstandard
name english "SURVIVAL"
rule ui_zm_gamemodegroup zsurvival
script zstandard
gametype zgrief
name english "TEAM VERSUS"
rule ui_zm_gamemodegroup zencounter
script zgrief
gametype zcleansed
name english "INFECTED"
rule ui_zm_gamemodegroup zencounter
script zcleansed
supercategory zm_buried
name english "WESTERN TOWN"
icon playlist_coop_zombie
filter playermatch
unlockxp 0
dlc 5
hideifmissingdlc
supercategory zm_prison
name english "ISLAND PRISON"
icon playlist_coop_zombie
filter playermatch
unlockxp 0
dlc 4
hideifmissingdlc
supercategory zm_highrise
name english "TOWERS"
icon playlist_coop_zombie
filter playermatch
unlockxp 0
dlc 3
hideifmissingdlc
supercategory zm_nuked
name english "DESERT"
icon playlist_coop_zombie
filter playermatch
unlockxp 0
dlc 2
hideifmissingdlc
supercategory zm_transit
name english "BUS ROUTE"
icon playlist_coop_zombie
filter playermatch
unlockxp 0
supercategory zm_tomb
name english "DIG SITE"
icon playlist_coop_zombie
filter playermatch
unlockxp 0
category transit
name english "BUS DEPOT"
icon playlist_coop_zombie
filter playermatch
supercategories zm_transit
unlockxp 0
category town
name english "TOWN"
icon playlist_coop_zombie
filter playermatch
supercategories zm_transit
unlockxp 0
category farm
name english "FARM"
icon playlist_coop_zombie
filter playermatch
supercategories zm_transit
unlockxp 0
category nuked
name english "TEST TOWN"
icon playlist_coop_zombie
filter playermatch
supercategories zm_nuked
unlockxp 0
dlc 2
hideifmissingdlc
category diner
name english "DINER"
icon playlist_coop_zombie
filter playermatch
supercategories zm_transit
unlockxp 0
dlc 3
hideifmissingdlc
category rooftop
name english "SKYSCRAPER"
icon playlist_coop_zombie
filter playermatch
supercategories zm_highrise
unlockxp 0
dlc 3
hideifmissingdlc
category prison
name english "PRISON YARD"
icon playlist_coop_zombie
filter playermatch
supercategories zm_prison
unlockxp 0
dlc 4
hideifmissingdlc
category cellblock
name english "CELL BLOCK"
icon playlist_coop_zombie
filter playermatch
supercategories zm_prison
unlockxp 0
dlc 4
hideifmissingdlc
category processing
name english "PROCESSING"
icon playlist_coop_zombie
filter playermatch
supercategories zm_buried
unlockxp 0
dlc 5
hideifmissingdlc
category street
name english "BOROUGH"
icon playlist_coop_zombie
filter playermatch
supercategories zm_buried
unlockxp 0
dlc 5
hideifmissingdlc
category tomb
name english "TRENCHES"
icon playlist_coop_zombie
filter playermatch
supercategories zm_tomb
unlockxp 0
dlc 6
hideifmissingdlc
playlist 1
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories transit
icon menu_zm_transit_zclassic_transit
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
zm_transit,zclassic,transit,1
playlist 2
name english "SURVIVAL"
description english "Survive with basic weapons and no upgrades."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories transit
icon menu_zm_transit_zsurvival_transit
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
zm_transit,zstandard,transit,1
playlist 3
name english "TEAM VERSUS"
description english "Two teams, one horde: the last team standing wins."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories town
icon menu_zm_transit_zencounter_town
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_restartMatchmakingTimer 15
zm_transit,zgrief,town,1
playlist 4
name english "SURVIVAL"
description english "Survive in the town, with upgrades available."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories town
icon menu_zm_transit_zsurvival_town
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
zm_transit,zstandard,town,1
playlist 5
name english "TEAM VERSUS"
description english "Two teams, one horde: the last team standing wins."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories farm
icon menu_zm_transit_zencounter_farm
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_restartMatchmakingTimer 15
zm_transit,zgrief,farm,1
playlist 6
name english "SURVIVAL"
description english "Survive at the farm, with a few weapons and upgrades."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories farm
icon menu_zm_transit_zsurvival_farm
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
zm_transit,zstandard,farm,1
playlist 7
name english "SURVIVAL"
description english "Survive in the small test town."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories nuked
dlc 2
hideifmissingdlc
icon menu_zm_nuked_zsurvival_nuked
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
zm_nuked,zstandard,nuked,1
playlist 8
name english "INFECTED"
description english "Play as the undead and hunt the last human."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories diner
dlc 3
hideifmissingdlc
icon menu_zm_transit_zencounter_diner
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_restartMatchmakingTimer 15
zm_transit_dr,zcleansed,diner,1
playlist 9
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories rooftop
dlc 3
hideifmissingdlc
icon menu_zm_highrise_zclassic_rooftop
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
zm_highrise,zclassic,rooftop,1
playlist 10
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories prison
dlc 4
hideifmissingdlc
icon menu_zm_prison_zclassic_prison
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
zm_prison,zclassic,prison,1
playlist 11
name english "TEAM VERSUS"
description english "Two teams, one horde: the last team standing wins."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories cellblock
dlc 4
hideifmissingdlc
icon menu_zm_prison_zencounter_cellblock
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_restartMatchmakingTimer 15
zm_prison,zgrief,cellblock,1
playlist 12
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories processing
dlc 5
hideifmissingdlc
icon menu_zm_buried_zclassic_processing
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
zm_buried,zclassic,processing,1
playlist 13
name english "TEAM VERSUS"
description english "Two teams, one horde: the last team standing wins."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories street
dlc 5
hideifmissingdlc
icon menu_zm_buried_zencounter_street
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_restartMatchmakingTimer 15
zm_buried,zgrief,street,1
playlist 14
name english "INFECTED"
description english "Play as the undead and hunt the last human."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories street
dlc 5
hideifmissingdlc
icon menu_zm_buried_zencounter_street
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_restartMatchmakingTimer 15
zm_buried,zcleansed,street,1
playlist 15
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 4
maxlocalplayers 1
categories tomb
dlc 6
hideifmissingdlc
icon menu_zm_tomb_zclassic_tomb
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 8
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 5
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
zm_tomb,zclassic,tomb,1
supercategory zm_buried_solo
name english "WESTERN TOWN"
icon playlist_coop_zombie
filter solomatch
unlockxp 0
dlc 5
hideifmissingdlc
supercategory zm_prison_solo
name english "ISLAND PRISON"
icon playlist_coop_zombie
filter solomatch
unlockxp 0
dlc 4
hideifmissingdlc
supercategory zm_highrise_solo
name english "TOWERS"
icon playlist_coop_zombie
filter solomatch
unlockxp 0
dlc 3
hideifmissingdlc
supercategory zm_nuked_solo
name english "DESERT"
icon playlist_coop_zombie
filter solomatch
unlockxp 0
dlc 2
hideifmissingdlc
supercategory zm_transit_solo
name english "BUS ROUTE"
icon playlist_coop_zombie
filter solomatch
unlockxp 0
supercategory zm_tomb_solo
name english "DIG SITE"
icon playlist_coop_zombie
filter solomatch
unlockxp 0
dlc 6
hideifmissingdlc
category transit_solo
name english "BUS DEPOT"
icon playlist_coop_zombie
filter solomatch
supercategories zm_transit_solo
unlockxp 0
category town_solo
name english "TOWN"
icon playlist_coop_zombie
filter solomatch
supercategories zm_transit_solo
unlockxp 0
category farm_solo
name english "FARM"
icon playlist_coop_zombie
filter solomatch
supercategories zm_transit_solo
unlockxp 0
category nuked_solo
name english "TEST TOWN"
icon playlist_coop_zombie
filter solomatch
supercategories zm_nuked_solo
unlockxp 0
dlc 2
hideifmissingdlc
category rooftop_solo
name english "ROOFTOP"
icon playlist_coop_zombie
filter solomatch
supercategories zm_highrise_solo
unlockxp 0
dlc 3
hideifmissingdlc
category prison_solo
name english "PRISON YARD"
icon playlist_coop_zombie
filter solomatch
supercategories zm_prison_solo
unlockxp 0
dlc 4
hideifmissingdlc
category cellblock_solo
name english "CELL BLOCK"
icon playlist_coop_zombie
filter solomatch
supercategories zm_prison_solo
unlockxp 0
dlc 4
hideifmissingdlc
category processing_solo
name english "PROCESSING"
icon playlist_coop_zombie
filter solomatch
supercategories zm_buried_solo
unlockxp 0
dlc 5
hideifmissingdlc
category street_solo
name english "BOROUGH"
icon playlist_coop_zombie
filter solomatch
supercategories zm_buried_solo
unlockxp 0
dlc 5
hideifmissingdlc
category tomb_solo
name english "TRENCHES"
icon playlist_coop_zombie
filter solomatch
supercategories zm_tomb_solo
unlockxp 0
dlc 6
hideifmissingdlc
playlist 91
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories transit_solo
icon menu_zm_transit_zclassic_transit
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
rule allowAllNAT 1
zm_transit,zclassic,transit,1
playlist 92
name english "SURVIVAL"
description english "Survive with basic weapons and no upgrades."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories transit_solo
icon menu_zm_transit_zsurvival_transit
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
rule allowAllNAT 1
zm_transit,zstandard,transit,1
playlist 93
name english "SURVIVAL"
description english "Survive in the town, with upgrades available."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories town_solo
icon menu_zm_transit_zsurvival_town
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
rule allowAllNAT 1
zm_transit,zstandard,town,1
playlist 94
name english "SURVIVAL"
description english "Survive at the farm, with a few weapons and upgrades."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories farm_solo
icon menu_zm_transit_zsurvival_farm
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
rule allowAllNAT 1
zm_transit,zstandard,farm,1
playlist 95
name english "SURVIVAL"
description english "Survive in the small test town."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories nuked_solo
dlc 2
hideifmissingdlc
icon menu_zm_nuked_zsurvival_nuked
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_autoteams 0
rule party_readyAllowOnlyLocalPlayers 1
rule allowAllNAT 1
zm_nuked,zstandard,nuked,1
playlist 96
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories rooftop_solo
dlc 3
hideifmissingdlc
icon menu_zm_highrise_zclassic_rooftop
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
rule allowAllNAT 1
zm_highrise,zclassic,rooftop,1
playlist 97
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories prison_solo
dlc 4
hideifmissingdlc
icon menu_zm_prison_zclassic_prison
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
rule allowAllNAT 1
zm_prison,zclassic,prison,1
playlist 98
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories processing_solo
dlc 5
hideifmissingdlc
icon menu_zm_buried_zclassic_processing
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
rule allowAllNAT 1
zm_buried,zclassic,processing,1
playlist 99
name english "STORY"
description english "Play the full story of this map."
unlockxp 0
maxparty 1
maxlocalplayers 1
categories tomb_solo
dlc 6
hideifmissingdlc
icon menu_zm_tomb_zclassic_tomb
rule xblive_rankedmatch 0
rule party_minplayers 1
rule party_maxplayers 1
rule party_matchedplayercount 0
rule party_readypercentrequired 0.7
rule party_vetoButtonVisible 0
rule party_minlobbytime 0
rule party_gameStartTimerLength 5
rule party_readyAllowOnlyLocalPlayers 1
rule party_autoteams 0
rule allowAllNAT 1
zm_tomb,zclassic,tomb,1
)PL";

constexpr char kPlaylistsMp[] =
    "version 1728\n"
    "settings\nsetdvar party_minplayers 1\n"
    "gametype hs_tdm\nname english \"TEAM DEATHMATCH\"\nscript tdm\n"
    "gametype hs_dom\nname english \"DOMINATION\"\nscript dom\n"
    "gametype hs_conf\nname english \"TAG COLLECT\"\nscript conf\n"
    "gametype hs_ffa\nname english \"FREE-FOR-ALL\"\nscript dm\n"
    "gametype hs_sd\nname english \"SEARCH & DESTROY\"\nscript sd\n"
    "gametype hs_ctf\nname english \"CAPTURE THE FLAG\"\nscript ctf\n"
    "gametype hs_koth\nname english \"MOVING ZONE\"\nscript koth\n"
    "gametype hs_hq\nname english \"HEADQUARTERS\"\nscript hq\n"
    "gametype hs_dem\nname english \"DEMOLITION\"\nscript dem\n"
    "gametype hs_sas\nname english \"MELEE MIX\"\nscript sas\n"
    "gametype hs_gun\nname english \"WEAPON LADDER\"\nscript gun\n"
    "gametype hs_oic\nname english \"ONE SHOT\"\nscript oic\n"
    "gametype hs_shrp\nname english \"WEAPON ROTATION\"\nscript shrp\n"
    "gametype hc_tdm\nname english \"TEAM DEATHMATCH\"\nscript tdm\nrule gts hardcoreMode 1\nrule gts friendlyfiretype 1\nrule gts playerHealthRegenTime 0\nrule gts playerMaxHealth 30\nrule gts allowKillcam 0\n"
    "gametype hc_sd\nname english \"SEARCH & DESTROY\"\nscript sd\nrule gts hardcoreMode 1\nrule gts friendlyfiretype 1\nrule gts playerHealthRegenTime 0\nrule gts playerMaxHealth 30\nrule gts allowKillcam 0\n"
    "gametype hc_ctf\nname english \"CAPTURE THE FLAG\"\nscript ctf\nrule gts hardcoreMode 1\nrule gts friendlyfiretype 1\nrule gts playerHealthRegenTime 0\nrule gts playerMaxHealth 30\nrule gts allowKillcam 0\n"
    "gametype hc_conf\nname english \"TAG COLLECT\"\nscript conf\nrule gts hardcoreMode 1\nrule gts friendlyfiretype 1\nrule gts playerHealthRegenTime 0\nrule gts playerMaxHealth 30\nrule gts allowKillcam 0\n"
    "gametype hc_dom\nname english \"DOMINATION\"\nscript dom\nrule gts hardcoreMode 1\nrule gts friendlyfiretype 1\nrule gts playerHealthRegenTime 0\nrule gts playerMaxHealth 30\nrule gts allowKillcam 0\n"
    "maplist all\nmp_carrier\nmp_dockside\nmp_drone\nmp_express\nmp_hijacked\nmp_la\nmp_meltdown\nmp_nightclub\nmp_overflow\nmp_raid\nmp_slums\nmp_socotra\nmp_turbine\nmp_village\n"
    "category core\nname english \"Core\"\nicon playlist_core\nfilter playermatch\nunlockxp 0\n"
    "category hardcore\nname english \"Hardcore\"\nicon playlist_hardcore\nfilter playermatch\nunlockxp 0\n"
    "category training\nname english \"Bot Training\"\nicon playlist_combat_training_obj\nfilter playermatch\nunlockxp 0\n"
    "category party\nname english \"Party\"\nicon playlist_party\nfilter playermatch\nunlockxp 0\n"
    "category league\nname english \"League\"\nicon league_moshpit\nfilter leaguematch\nunlockxp 0\n"
    "playlist 1\nname english \"Team Deathmatch\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_tdm\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_tdm,1\nall,hs_tdm,1\n"
    "playlist 4\nname english \"Free-For-All\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_ffa\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_ffa,1\nall,hs_ffa,1\n"
    "playlist 5\nname english \"Domination\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_domination\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_dom,1\nall,hs_dom,1\n"
    "playlist 7\nname english \"Demolition\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_demolition\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_dem,1\nall,hs_dem,1\n"
    "playlist 8\nname english \"Tag Collect\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_kill_confirm\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_conf,1\nall,hs_conf,1\n"
    "playlist 9\nname english \"Moving Zone\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_koth\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_koth,1\nall,hs_koth,1\n"
    "playlist 10\nname english \"Headquarters\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_headquarters\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_hq,1\nall,hs_hq,1\n"
    "playlist 11\nname english \"Capture The Flag\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_ctf\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_ctf,1\nall,hs_ctf,1\n"
    "playlist 12\nname english \"Search & Destroy\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories core\nicon playlist_search_destroy\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_sd,1\nall,hs_sd,1\n"
    "playlist 41\nname english \"Team Deathmatch\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories hardcore\nicon playlist_tdm\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hc_tdm,1\nall,hc_tdm,1\n"
    "playlist 43\nname english \"Search & Destroy\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories hardcore\nicon playlist_search_destroy\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hc_sd,1\nall,hc_sd,1\n"
    "playlist 44\nname english \"Capture The Flag\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories hardcore\nicon playlist_ctf\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hc_ctf,1\nall,hc_ctf,1\n"
    "playlist 45\nname english \"Tag Collect\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories hardcore\nicon playlist_kill_confirm\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hc_conf,1\nall,hc_conf,1\n"
    "playlist 46\nname english \"Domination\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories hardcore\nicon playlist_domination\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hc_dom,1\nall,hc_dom,1\n"
    "playlist 58\nname english \"Objective\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories training\nicon playlist_combat_training_obj\nrule bot_enemies 1\nrule bot_UseFriendNames 0\nrule scr_disableChallenges 1\nrule scr_disableStatTracking 1\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_tdm,1\nall,hs_tdm,1\nmp_nuketown_2020,hs_dom,1\nall,hs_dom,1\nmp_nuketown_2020,hs_conf,1\nall,hs_conf,1\n"
    "playlist 64\nname english \"Party Mix\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories party\nicon playlist_sticks_and_stones\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_sas,1\nall,hs_sas,1\nmp_nuketown_2020,hs_gun,1\nall,hs_gun,1\nmp_nuketown_2020,hs_oic,1\nall,hs_oic,1\nmp_nuketown_2020,hs_shrp,1\nall,hs_shrp,1\n"
    "playlist 89\nname english \"Ranked Mix\"\nunlockxp 0\nmaxparty 6\nmaxlocalplayers 1\ncategories league\nleagueid 10\nicon league_moshpit\nrule party_minplayers 1\nrule party_maxplayers 12\nmp_nuketown_2020,hs_tdm,1\nall,hs_tdm,1\nmp_nuketown_2020,hs_dom,1\nall,hs_dom,1\nmp_nuketown_2020,hs_ctf,1\nall,hs_ctf,1\n";

// Multiplayer, online only (session mode without the Zombies bit 0x10, 0x668ee0): entry "leagues.info", JSON read by
// 0x875850 (jsmn-style tokenizer 0x6e2640), and the menu needs 0x539c40 to accept it: "version" > 0 (0xd525d8), at
// least one league (0x2501220, 0x4c1490) and one division (0x2501224, 0x5ba230). The structure of the shipped file, English only (P1046).
constexpr char kLeaguesMp[] = R"LG({"version":23,"divisions":[{"id":0,"name":"Unplaced","icons":[{"rank":0,"large":"menu_div_no_place","small":"whitetransparent"}],"can_place":false},{"id":1,"name":"Division 1","icons":[{"rank":10,"large":"menu_div_iron","small":"menu_div_iron_64"},{"rank":25,"large":"menu_div_iron_sub03","small":"menu_div_iron_sub03_64"},{"rank":50,"large":"menu_div_iron_sub02","small":"menu_div_iron_sub02_64"},{"rank":0,"large":"menu_div_iron_sub01","small":"menu_div_iron_sub01_64"}],"skill_min":-10,"skill_avg":-0.6189626,"skill_max":-0.2355918,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":2,"name":"Division 2","icons":[{"rank":10,"large":"menu_div_bronze","small":"menu_div_bronze_64"},{"rank":25,"large":"menu_div_bronze_sub03","small":"menu_div_bronze_sub03_64"},{"rank":50,"large":"menu_div_bronze_sub02","small":"menu_div_bronze_sub02_64"},{"rank":0,"large":"menu_div_bronze_sub01","small":"menu_div_bronze_sub01_64"}],"skill_min":-0.2355918,"skill_avg":0.0408454,"skill_max":0.2770509,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":3,"name":"Division 3","icons":[{"rank":10,"large":"menu_div_silver","small":"menu_div_silver_64"},{"rank":25,"large":"menu_div_silver_sub03","small":"menu_div_silver_sub03_64"},{"rank":50,"large":"menu_div_silver_sub02","small":"menu_div_silver_sub02_64"},{"rank":0,"large":"menu_div_silver_sub01","small":"menu_div_silver_sub01_64"}],"skill_min":0.2770509,"skill_avg":0.4978264,"skill_max":0.718602,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":4,"name":"Division 4","icons":[{"rank":10,"large":"menu_div_gold","small":"menu_div_gold_64"},{"rank":25,"large":"menu_div_gold_sub03","small":"menu_div_gold_sub03_64"},{"rank":50,"large":"menu_div_gold_sub02","small":"menu_div_gold_sub02_64"},{"rank":0,"large":"menu_div_gold_sub01","small":"menu_div_gold_sub01_64"}],"skill_min":0.718602,"skill_avg":0.9548074,"skill_max":1.2312447,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":5,"name":"Division 5","icons":[{"rank":10,"large":"menu_div_platinum","small":"menu_div_platinum_64"},{"rank":25,"large":"menu_div_platinum_sub03","small":"menu_div_platinum_sub03_64"},{"rank":50,"large":"menu_div_platinum_sub02","small":"menu_div_platinum_sub02_64"},{"rank":0,"large":"menu_div_platinum_sub01","small":"menu_div_platinum_sub01_64"}],"skill_min":1.2312447,"skill_avg":1.5666661,"skill_max":2.2875353,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":6,"name":"Top Division","icons":[{"rank":10,"large":"menu_div_semipro","small":"menu_div_semipro_64"},{"rank":25,"large":"menu_div_semipro_sub03","small":"menu_div_semipro_sub03_64"},{"rank":50,"large":"menu_div_semipro_sub02","small":"menu_div_semipro_sub02_64"},{"rank":0,"large":"menu_div_semipro_sub01","small":"menu_div_semipro_sub01_64"}],"skill_min":2.2875353,"skill_avg":2.5250876,"skill_max":10.0,"subdivision_size":200,"tweak_skill":true,"can_place":false},{"id":7,"name":"Division 1","icons":[{"rank":10,"large":"menu_div_iron","small":"menu_div_iron_64"},{"rank":25,"large":"menu_div_iron_sub03","small":"menu_div_iron_sub03_64"},{"rank":50,"large":"menu_div_iron_sub02","small":"menu_div_iron_sub02_64"},{"rank":0,"large":"menu_div_iron_sub01","small":"menu_div_iron_sub01_64"}],"skill_min":-10,"skill_avg":-0.0456818,"skill_max":0.3979224,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":8,"name":"Division 2","icons":[{"rank":10,"large":"menu_div_bronze","small":"menu_div_bronze_64"},{"rank":25,"large":"menu_div_bronze_sub03","small":"menu_div_bronze_sub03_64"},{"rank":50,"large":"menu_div_bronze_sub02","small":"menu_div_bronze_sub02_64"},{"rank":0,"large":"menu_div_bronze_sub01","small":"menu_div_bronze_sub01_64"}],"skill_min":0.3979224,"skill_avg":0.7177922,"skill_max":0.9911092,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":9,"name":"Division 3","icons":[{"rank":10,"large":"menu_div_silver","small":"menu_div_silver_64"},{"rank":25,"large":"menu_div_silver_sub03","small":"menu_div_silver_sub03_64"},{"rank":50,"large":"menu_div_silver_sub02","small":"menu_div_silver_sub02_64"},{"rank":0,"large":"menu_div_silver_sub01","small":"menu_div_silver_sub01_64"}],"skill_min":0.9911092,"skill_avg":1.246572,"skill_max":1.5020348,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":10,"name":"Division 4","icons":[{"rank":10,"large":"menu_div_gold","small":"menu_div_gold_64"},{"rank":25,"large":"menu_div_gold_sub03","small":"menu_div_gold_sub03_64"},{"rank":50,"large":"menu_div_gold_sub02","small":"menu_div_gold_sub02_64"},{"rank":0,"large":"menu_div_gold_sub01","small":"menu_div_gold_sub01_64"}],"skill_min":1.5020348,"skill_avg":1.7753518,"skill_max":2.0952216,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":11,"name":"Division 5","icons":[{"rank":10,"large":"menu_div_platinum","small":"menu_div_platinum_64"},{"rank":25,"large":"menu_div_platinum_sub03","small":"menu_div_platinum_sub03_64"},{"rank":50,"large":"menu_div_platinum_sub02","small":"menu_div_platinum_sub02_64"},{"rank":0,"large":"menu_div_platinum_sub01","small":"menu_div_platinum_sub01_64"}],"skill_min":2.0952216,"skill_avg":2.4833429,"skill_max":3.3174718,"subdivision_size":200,"tweak_skill":true,"can_place":true},{"id":12,"name":"Top Division","icons":[{"rank":10,"large":"menu_div_semipro","small":"menu_div_semipro_64"},{"rank":25,"large":"menu_div_semipro_sub03","small":"menu_div_semipro_sub03_64"},{"rank":50,"large":"menu_div_semipro_sub02","small":"menu_div_semipro_sub02_64"},{"rank":0,"large":"menu_div_semipro_sub01","small":"menu_div_semipro_sub01_64"}],"skill_min":3.3174718,"skill_avg":3.5923472,"skill_max":10.0,"subdivision_size":200,"tweak_skill":true,"can_place":false}],"numbers":["Zero","One","Two","Three","Four","Five","Six","Seven","Eight","Nine"],"nouns":["Alpha","Ace","Apollo","Banshee","Battle","Bear","Blue","Boomslang","Bravo","Charlie","Checkmate","Cobra","Crimson","Domino","Delta","Eagle","Echo","Falcon","Fox","Foxtrot","Frost","Giant","Gemini","Griffin","Hawk","Havoc","Ice","Indigo","Jester","Jupiter","Kilo","King","Knight","Lion","Lightning","Mars","Mercury","Midnight","Mike","Mongoose","Montana","Odin","Omaha","Omega","Oscar","Panther","Papa","Pharaoh","Phantom","Phoenix","Pluto","Python","Ranger","Rattlesnake","Raven","Red","Reaper","Rook","Saturn","Shark","Shield","Sidewinder","Scorpion","Sierra","Spartan","Spider","Stingray","Strike","Storm","Sword","Tango","Thunder","Tiger","Titan","Trident","Utah","Victor","Viper","Voodoo","Warrior","Werewolf","Whiskey","Wolf","X-Ray","Yankee","Zombie","Zulu"],"leagues":[{"name":"Solo","description":"Solo league","id":1,"icon":"league_champions","teams_allowed":false,"team_size":1,"randoms_allowed":true,"bonus_pool":600,"bonus_per_win":200,"bonus_time":"2012-05-16 10:00","elo_mean":5000,"elo_scale":3000,"elo_k":200,"elo_awards":[10,30,50,70,90,110,130,150,170,190],"placement_games":3,"placement_window":0.1,"divisions":[1,2,3,4,5,6]},{"name":"Duo","description":"Duo league","id":2,"icon":"league_champions","teams_allowed":true,"team_size":2,"randoms_allowed":true,"bonus_pool":600,"bonus_per_win":200,"bonus_time":"2012-05-16 10:00","elo_mean":5000,"elo_scale":3000,"elo_k":200,"elo_awards":[10,30,50,70,90,110,130,150,170,190],"placement_games":3,"placement_window":0.1,"divisions":[1,2,3,4,5,6]},{"name":"Ranked Mix","description":"Ranked mix","id":10,"icon":"league_moshpit","teams_allowed":false,"team_size":6,"randoms_allowed":true,"bonus_cumulative":true,"bonus_pool":200,"bonus_per_win":100,"bonus_days":1,"promotion_amnesty":7,"demotion_amnesty":100,"elo_mean":5000,"elo_scale":3000,"elo_k":200,"elo_awards":[10,30,50,70,90,110,130,150,170,190],"placement_games":5,"placement_window":0.2,"divisions":[1,2,3,4,5,6],"seasons":[{"id":37,"name":"Season 23","start":"2014-10-01 09:00","end":"2014-11-03 09:00"},{"id":38,"name":"Season 24","start":"2014-11-05 09:00","end":"2014-12-01 09:00"},{"id":39,"name":"Season 25","start":"2014-12-03 09:00","end":"2014-12-29 09:00"},{"id":40,"name":"Season 26","start":"2015-01-03 09:00","end":"2015-02-01 09:00"},{"id":41,"name":"Season 27","start":"2015-02-03 09:00","end":"2015-03-01 09:00"},{"id":42,"name":"Season 28","start":"2015-03-03 09:00","end":"2015-04-01 09:00"},{"id":43,"name":"Season 29","start":"2015-04-03 09:00","end":"2015-05-01 09:00"},{"id":44,"name":"Season 30","start":"2015-05-03 09:00","end":"2015-06-01 09:00"},{"id":45,"name":"Season 31","start":"2015-06-03 09:00","end":"2015-07-01 09:00"},{"id":46,"name":"Season 32","start":"2015-07-03 09:00","end":"2015-08-01 09:00"},{"id":47,"name":"Season 33","start":"2015-08-03 09:00","end":"2015-09-01 09:00"},{"id":48,"name":"Season 34","start":"2015-09-03 09:00","end":"2015-10-01 09:00"},{"id":49,"name":"Season 35","start":"2015-10-03 09:00","end":"2015-11-01 09:00"},{"id":50,"name":"Season 36","start":"2015-11-03 09:00","end":"2015-12-01 09:00"}]},{"name":"Teams","description":"Team league","id":11,"icon":"league_champions","teams_allowed":true,"team_size":4,"randoms_allowed":true,"bonus_cumulative":true,"bonus_pool":200,"bonus_per_win":100,"bonus_days":1,"promotion_amnesty":7,"demotion_amnesty":100,"elo_mean":5000,"elo_scale":3000,"elo_k":200,"elo_awards":[10,30,50,70,90,110,130,150,170,190],"placement_games":5,"placement_window":0.2,"divisions":[7,8,9,10,11,12],"seasons":[{"id":37,"name":"Season 23","start":"2014-10-01 09:00","end":"2014-11-03 09:00"},{"id":38,"name":"Season 24","start":"2014-11-05 09:00","end":"2014-12-01 09:00"},{"id":39,"name":"Season 25","start":"2014-12-03 09:00","end":"2014-12-29 09:00"},{"id":40,"name":"Season 26","start":"2015-01-03 09:00","end":"2015-02-01 09:00"},{"id":41,"name":"Season 27","start":"2015-02-03 09:00","end":"2015-03-01 09:00"},{"id":42,"name":"Season 28","start":"2015-03-03 09:00","end":"2015-04-01 09:00"},{"id":43,"name":"Season 29","start":"2015-04-03 09:00","end":"2015-05-01 09:00"},{"id":44,"name":"Season 30","start":"2015-05-03 09:00","end":"2015-06-01 09:00"},{"id":45,"name":"Season 31","start":"2015-06-03 09:00","end":"2015-07-01 09:00"},{"id":46,"name":"Season 32","start":"2015-07-03 09:00","end":"2015-08-01 09:00"},{"id":47,"name":"Season 33","start":"2015-08-03 09:00","end":"2015-09-01 09:00"},{"id":48,"name":"Season 34","start":"2015-09-03 09:00","end":"2015-10-01 09:00"},{"id":49,"name":"Season 35","start":"2015-10-03 09:00","end":"2015-11-01 09:00"},{"id":50,"name":"Season 36","start":"2015-11-03 09:00","end":"2015-12-01 09:00"}]},{"name":"Team Deathmatch","description":"Team deathmatch league","id":12,"icon":"league_moshpit","teams_allowed":false,"team_size":6,"randoms_allowed":true,"bonus_cumulative":true,"bonus_pool":200,"bonus_per_win":100,"bonus_days":1,"promotion_amnesty":7,"demotion_amnesty":100,"elo_mean":5000,"elo_scale":3000,"elo_k":200,"elo_awards":[10,30,50,70,90,110,130,150,170,190],"placement_games":5,"placement_window":0.2,"divisions":[1,2,3,4,5,6],"seasons":[{"id":37,"name":"Season 23","start":"2014-10-01 09:00","end":"2014-11-03 09:00"},{"id":38,"name":"Season 24","start":"2014-11-05 09:00","end":"2014-12-01 09:00"},{"id":39,"name":"Season 25","start":"2014-12-03 09:00","end":"2014-12-29 09:00"},{"id":40,"name":"Season 26","start":"2015-01-03 09:00","end":"2015-02-01 09:00"},{"id":41,"name":"Season 27","start":"2015-02-03 09:00","end":"2015-03-01 09:00"},{"id":42,"name":"Season 28","start":"2015-03-03 09:00","end":"2015-04-01 09:00"},{"id":43,"name":"Season 29","start":"2015-04-03 09:00","end":"2015-05-01 09:00"},{"id":44,"name":"Season 30","start":"2015-05-03 09:00","end":"2015-06-01 09:00"},{"id":45,"name":"Season 31","start":"2015-06-03 09:00","end":"2015-07-01 09:00"},{"id":46,"name":"Season 32","start":"2015-07-03 09:00","end":"2015-08-01 09:00"},{"id":47,"name":"Season 33","start":"2015-08-03 09:00","end":"2015-09-01 09:00"},{"id":48,"name":"Season 34","start":"2015-09-03 09:00","end":"2015-10-01 09:00"},{"id":49,"name":"Season 35","start":"2015-10-03 09:00","end":"2015-11-01 09:00"},{"id":50,"name":"Season 36","start":"2015-11-03 09:00","end":"2015-12-01 09:00"}]}]})LG";

// Entry "codtv-mp.json" ("codtv-zm.json" in Zombies), parsed by the "CODTV:" reader: without it the COD TV carousel and the emblem editor, which
// draw their menus from it by "ref", show MENU_FILESHARE_ERROR. Version 7 is what the reader requires. No videos: they
// came from servers that are gone.
constexpr char kCodtvMp[] = R"({"version":7,"menus":[
{"ref":"recents","name":{"ref":"MENU_SELECT_FILM"},"type":"folder","content":[
 {"name":{"ref":"MENU_RECENT_GAMES"},"type":"dwfolder","filters":["self","pooled"]},
 {"name":{"ref":"MENU_FILMS"},"type":"dwfolder","filters":["self","user","films"]},
 {"name":{"ref":"MENU_EDITED_FILMS"},"type":"dwfolder","filters":["self","user","clips"]}]},
{"ref":"community","name":{"ref":"MENU_COD_TV_CAPS"},"type":"folder","content":[
 {"name":{"ref":"MENU_MY_CHANNEL"},"type":"folder","content":[
  {"name":{"ref":"MENU_MY_CHANNEL"},"type":"folder","imgCategory":"material","imgName":"menu_codtv_icon_mychannel","content":[
   {"name":{"ref":"MENU_RECENT_GAMES"},"type":"dwfolder","filters":["self","pooled"]},
   {"name":{"ref":"MENU_FILMS"},"type":"dwfolder","filters":["self","user","films"]},
   {"name":{"ref":"MENU_EDITED_FILMS"},"type":"dwfolder","filters":["self","user","clips"]},
   {"name":{"ref":"MENU_SCREENSHOTS"},"type":"dwfolder","filters":["self","user","screenshots"]},
   {"name":{"ref":"MENU_EMBLEMS_CAPS"},"type":"dwfolder","filters":["self","user","emblems"]},
   {"name":{"ref":"MENU_CUSTOMGAMES_CAPS"},"type":"dwfolder","filters":["self","user","customgames"]}]},
  {"name":{"ref":"MENU_FILEMANAGER"},"type":"custombutton","imgCategory":"material","imgName":"menu_codtv_icon_fileshare","action":"filemanager"}]}]},
{"ref":"playerchannel","name":{"ref":"MENU_PLAYER_CHANNEL"},"type":"folder","content":[
 {"name":{"ref":"MENU_RECENT_GAMES"},"type":"dwfolder","filters":["otherplayer","pooled"]},
 {"name":{"ref":"MENU_EMBLEMS_CAPS"},"type":"dwfolder","filters":["otherplayer","user","emblems"]}]},
{"ref":"combatrecord","name":{"ref":"MENU_COMBAT_RECORD_CAPS"},"type":"folder","content":
 {"name":{"ref":"MENU_SCREENSHOTS"},"type":"dwfolder","filters":["self","user","screenshots"]}},
{"ref":"emblems","name":{"ref":"MENU_EMBLEMS_CAPS"},"type":"folder","content":[
 {"name":{"ref":"MENU_EMBLEMS_CAPS"},"type":"dwfolder","filters":["self","user","emblems"]},
 {"name":{"ref":"MENU_NEW_EMBLEM"},"type":"folder","content":[
  {"name":{"ref":"MENU_NEW_EMBLEM"},"type":"custombutton","imgCategory":"material","imgName":"menu_mp_emblem_new_icon","action":"newemblem"}]}]},
{"ref":"leagueidentity","name":{"ref":"MENU_LEAGUE_IDENTITY_CAPS"},"type":"folder","content":
 {"name":{"ref":"MENU_EMBLEMS_CAPS"},"type":"dwfolder","filters":["leagueteam","user","emblems"]}},
{"ref":"ingamestore","name":{"ref":"MENU_INGAMESTORE"},"type":"folder","content":[
 {"name":{"ref":"MENU_STORE_MAP_PACKS_CATEGORY"},"type":"dwfolder","filters":["ingamestore","mappacks"]}]},
{"ref":"customgames","name":{"ref":"MENU_CUSTOMGAMES_CAPS"},"type":"folder","content":
 {"name":{"ref":"MENU_CUSTOMGAMES_CAPS"},"type":"dwfolder","filters":["self","user","customgames"]}}]})";

struct WadEntry { const char* name; const char* text; size_t len; };

std::vector<uint8_t> online_wad(std::initializer_list<WadEntry> entries) {
    std::vector<uint8_t> w(0x10 + 0x2c * entries.size(), 0);
    auto be32 = [&w](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) w[at + i] = (uint8_t)(v >> (24 - 8 * i)); };
    be32(0, 0x543377ab);
    be32(8, (uint32_t)entries.size());
    be32(0xc, 0xffffffff);
    size_t at = 0x10;
    for (const WadEntry& e : entries) {
        std::vector<uint8_t> z(compressBound(e.len));
        uLongf zlen = z.size();
        compress2(z.data(), &zlen, (const Bytef*)e.text, e.len, Z_BEST_COMPRESSION);
        strncpy((char*)&w[at], e.name, 0x1f);
        be32(at + 0x20, (uint32_t)zlen);
        be32(at + 0x24, (uint32_t)e.len);
        be32(at + 0x28, (uint32_t)w.size());
        w.insert(w.end(), z.begin(), z.begin() + zlen);
        at += 0x2c;
    }
    return w;
}

// Multiplayer publisher files "lsssk0" and "lsssk1" (older t6mp build: 0x665060 names them, 0x922bb0 hands each one's content to
// its check: 0x922ac0, then 0x922b30). Both import it with LibTomCrypt's ecc_import (0xa65650: key of 0x1c bytes,
// DER SEQUENCE {BIT STRING flag (1 = private), SHORT INTEGER size, INTEGER x, INTEGER y [, INTEGER k]}, size looked up
// in the curve table at 0xcda018) and the game stops on ERROR6329 when the import fails: lsssk0 must be a public key,
// lsssk1 a private one. A key pair of our own, P-256 (size 32), made for this responder: nothing it protects is
// anyone's, the game only checks that the two files are keys.
constexpr uint8_t kLssskPrivate[] = {
    0x30, 0x6f, 0x03, 0x02, 0x07, 0x80, 0x02, 0x01, 0x20, 0x02, 0x21, 0x00, 0xb9, 0xef, 0x4c, 0x3d, 0xce, 0x4d, 0x43,
    0x40, 0x20, 0x86, 0xc9, 0x90, 0xb5, 0x97, 0x19, 0x0b, 0x40, 0x29, 0x23, 0x77, 0x3e, 0xa3, 0x58, 0x32, 0x86, 0x1c,
    0xde, 0x72, 0xcf, 0x39, 0x9d, 0xf8, 0x02, 0x21, 0x00, 0xe1, 0x27, 0x78, 0x29, 0xd6, 0xc5, 0x61, 0x68, 0xf9, 0x8a,
    0x0d, 0xb0, 0x1a, 0x5d, 0xcf, 0x6e, 0x03, 0x65, 0x0a, 0xa4, 0xcc, 0xc9, 0xa6, 0xc7, 0x31, 0x6e, 0x16, 0x2e, 0x56,
    0xd3, 0xb4, 0xa4, 0x02, 0x20, 0x0b, 0x5b, 0x07, 0xf5, 0x11, 0x52, 0x47, 0x83, 0xd7, 0x71, 0x72, 0x50, 0xf1, 0xb4,
    0x8b, 0xda, 0x27, 0xfd, 0x9b, 0x0f, 0xc6, 0x21, 0x0f, 0x48, 0x0c, 0xa6, 0x46, 0x4e, 0x2e, 0x2b, 0x70, 0x77,
};
constexpr size_t LSSSK_PUBLIC_LEN = 0x4f;   // the same sequence without k: shorter, flag 0

std::vector<uint8_t> lsssk(bool is_private) {
    if (is_private) return {kLssskPrivate, kLssskPrivate + sizeof kLssskPrivate};
    std::vector<uint8_t> k(kLssskPrivate, kLssskPrivate + LSSSK_PUBLIC_LEN);
    k[1] = LSSSK_PUBLIC_LEN - 2;
    k[5] = 0;
    return k;
}

// Zone of the day (ffotd). The client reads it as publisher files "<zone>.ff.%02d" (0x553660), each part landing in a
// 0x20000 buffer (0x2f8d690) and ending in a 16-byte trailer checked by 0x91f970: "SKID" (0xbe5968), u32 A (only
// compared between parts), u32 total size in host order (≤ 0x1ffe0, read back by 0x457310; the converter 0x6972e0 ->
// [0x2aee854] = 0x9b0a60 is the identity, set at 0x6b5584), 2 bytes, part count,
// part index. The zone is the player's own file in zone/all, served whole as part 0; Zombies scripts link against
// it (_globallogic.gsc lives there only). A zone missing from the install gets 0 results, as before.
extern "C" const char* win32_game_dir();
constexpr size_t FFOTD_MAX = 0x1ffe0;
std::vector<uint8_t> ffotd_part(const std::string& params) {
    const size_t a = params.find("ffotd_"), e = params.find(".ff.", a == std::string::npos ? 0 : a);
    if (a == std::string::npos || e == std::string::npos || atoi(params.c_str() + e + 4) != 0) return {};
    const std::string path = std::string(win32_game_dir()) + "/zone/all/" + params.substr(a, e + 3 - a);
    std::vector<uint8_t> z;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        uint8_t buf[0x4000];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) z.insert(z.end(), buf, buf + n);
        fclose(f);
    }
    if (z.empty() || z.size() > FFOTD_MAX) return {};
    const uint32_t n = (uint32_t)z.size();
    const uint8_t trailer[16] = {'S', 'K', 'I', 'D', 0, 0, 0, 0, (uint8_t)n, (uint8_t)(n >> 8),
                                 (uint8_t)(n >> 16), (uint8_t)(n >> 24), 0, 0, 1, 0};
    z.insert(z.end(), trailer, trailer + sizeof trailer);
    return z;
}

// The player's own files (stats, classes): kept whole under players/demonware_steam.
std::string user_file_path(const std::string& name) {   // "" for anything but a plain file name
    if (name.empty() || name.size() > 64 || name[0] == '.' ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos)
        return "";
    return std::string(win32_game_dir()) + "/players/demonware_steam/" + name;
}
std::vector<uint8_t> read_user_file(const std::string& name) {
    std::vector<uint8_t> data;
    const std::string path = user_file_path(name);
    if (FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "rb")) {
        uint8_t buf[0x4000];
        for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
        fclose(f);
    }
    return data;
}
void keep_user_file(const std::string& name, const uint8_t* data, size_t n) {
    const std::string path = user_file_path(name), tmp = path + ".tmp";
    if (path.empty() || !n) return;
    mkdir((std::string(win32_game_dir()) + "/players/demonware_steam").c_str(), 0755);
    FILE* f = fopen(tmp.c_str(), "wb");   // whole, then renamed: a cut file would come back as the stats
    if (!f) return;
    const bool written = fwrite(data, 1, n, f) == n;
    if (fclose(f) == 0 && written) rename(tmp.c_str(), path.c_str());
}

void lobby_task(uint32_t s, Sock& k, const std::vector<uint8_t>& pt) {
    static uint64_t transaction = 0;
    std::vector<uint8_t> b;
    auto typed = [&b](uint8_t type, uint64_t v, int bytes) {
        b.push_back(type);
        for (int i = 0; i < bytes; i++) b.push_back((uint8_t)(v >> 8 * i));
    };
    const int task = pt.size() > 2 && pt[1] == 3 ? pt[2] : -1;
    const std::string params(pt.begin(), pt.end());
    std::vector<uint8_t> file;
    if (pt[0] == SVC_FILES && task == TASK_READ_PUBLISHER_FILE && params.find(".wad") != std::string::npos)
        file = params.find("_zm_") != std::string::npos
                   // The entry read is named after the session mode at that moment (0x5f0da0): both names.
                   ? online_wad({{"playlists_zm.info", kPlaylistsZm, sizeof kPlaylistsZm},   // with its NUL
                                 {"playlists_mp.info", kPlaylistsZm, sizeof kPlaylistsZm},
                                 {"codtv-zm.json", kCodtvMp, sizeof kCodtvMp - 1}})
                   : online_wad({{"playlists_mp.info", kPlaylistsMp, sizeof kPlaylistsMp}, {"leagues.info", kLeaguesMp, sizeof kLeaguesMp - 1},
                                 {"codtv-mp.json", kCodtvMp, sizeof kCodtvMp - 1}});
    else if (pt[0] == SVC_FILES && task == TASK_READ_PUBLISHER_FILE && params.find("lsssk") != std::string::npos)
        file = lsssk(params.find("lsssk1") != std::string::npos);
    else if (pt[0] == SVC_FILES && task == TASK_READ_PUBLISHER_FILE && params.find("ffotd_") != std::string::npos)
        file = ffotd_part(params);
    // Read and upload of a player's file: [10][03 task][0x10 name NUL]; the upload follows with [01 flag][0x13][08 u32 size][bytes].
    const bool user_read = pt[0] == SVC_FILES && task == TASK_READ_USER_FILE, user_write = pt[0] == SVC_FILES && task == TASK_WRITE_USER_FILE;
    const std::string user_name = (user_read || user_write) && pt.size() > 4 ? std::string(params.c_str() + 4) : "";
    if (user_read) file = read_user_file(user_name);
    if (user_write && !user_name.empty()) {
        const size_t at = 4 + user_name.size() + 1 + 2 + 6;
        uint32_t n = 0;
        if (at <= pt.size()) memcpy(&n, &pt[at - 4], 4);
        if (at <= pt.size() && n <= pt.size() - at) keep_user_file(user_name, &pt[at], n);
    }
    typed(0xa, ++transaction, 8);
    if (user_read && file.empty()) {   // no such file: the game makes a new profile
        typed(8, 1000, 4);
        reply(k, MSG_LOBBY_TASK_REPLY, b);
        if (g_trace) fprintf(stderr, "[DWP] salon s=%u: fichier joueur \"%s\" absent (1000)\n", s, user_name.c_str());
        return;
    }
    typed(8, 0, 4);
    typed(3, 0, 1);
    const bool server_time = pt[0] == SVC_TITLE && task == TASK_SERVER_TIME, geo = pt[0] == SVC_DML && task == TASK_USER_DATA;
    const bool league = pt[0] == SVC_LEAGUE && (task == TASK_LEAGUE_TEAM_ID || task == TASK_LEAGUE_SUBDIVISION || task == TASK_LEAGUE_TEAM_INFO);
    typed(8, file.empty() && !server_time && !geo && !league ? 0 : 1, 4);
    if (!file.empty()) {
        typed(8, 1, 4);
        typed(0x13, 0, 0);
        typed(8, file.size(), 4);
        b.insert(b.end(), file.begin(), file.end());
    } else if (server_time) {
        typed(8, 1, 4);
        typed(8, (uint32_t)time(nullptr), 4);
    } else if (geo) {
        typed(8, 1, 4);
        for (const char* v : {"ZZ", "Unknown", "Unknown", "Unknown"}) {
            typed(0x10, 0, 0);
            b.insert(b.end(), v, v + strlen(v) + 1);
        }
        typed(13, 0, 4);
        typed(13, 0, 4);
    } else if (league) {
        typed(8, 1, 4);
        typed(0xa, LEAGUE_TEAM_ID, 8);
        if (task == TASK_LEAGUE_TEAM_INFO) {   // 0x9f4480: id, name, blob, u32
            const uint32_t svc = kVa.auth_service_slot ? win32_mem_read32(kVa.auth_service_slot) : 0;
            const std::string name = svc ? get_cstr(svc + AUTH_USER_NAME) : "";
            typed(0x10, 0, 0);
            b.insert(b.end(), name.c_str(), name.c_str() + name.size() + 1);
            typed(0x13, 0, 0);
            typed(8, 0, 4);
            typed(8, 0, 4);
        } else if (task == TASK_LEAGUE_SUBDIVISION) {   // 0x9f30f0: team, league, subdivision, meta blob
            typed(0xa, LEAGUE_RANKED_ID, 8);
            typed(0xa, LEAGUE_SUBDIVISION_ID, 8);
            static constexpr char meta[] = "1,0";   // version 1, division 0: preseason (0x68ccb0)
            typed(0x13, 0, 0);
            typed(8, sizeof meta, 4);
            b.insert(b.end(), meta, meta + sizeof meta);
        }
    }
    reply(k, MSG_LOBBY_TASK_REPLY, b);
    if (g_trace) fprintf(stderr, "[DWP] salon s=%u: service %u tâche %d → fait, %s\n", s, pt[0], task,
                         file.empty() ? "0 résultat" : "1 fichier");
}

// One frame from the client: [flag][type][body] in clear, or [flag 1][u32 counter][3DES: mac4, type, body, pad].
void on_frame(uint32_t s, Sock& k, std::vector<uint8_t> f) {
    if (f.empty()) return;
    std::vector<uint8_t> pt;
    if (f[0] == 0) pt.assign(f.begin() + 1, f.end());
    else {
        if (f.size() < 5 + 8 || !g_have_session_key || !tiger_ready()) { hexdump("chiffre-illisible", s, f.data(), f.size()); return; }
        uint8_t iv[8];
        std::vector<uint8_t> ct(f.begin() + 5, f.end());
        if (!iv_of(le32(&f[1]), iv) || !des3_cbc(false, g_session_key, iv, ct) || ct.size() < 5) return;
        pt.assign(ct.begin() + 4, ct.end());
    }
    if (pt.empty()) return;
    std::string host = name_of(k.peer_ip);
    hexdump(f[0] ? "recu-dechiffre" : "recu", s, pt.data(), pt.size());
    if (host.find("-auth.") != std::string::npos && pt[0] == MSG_AUTH_STEAM) auth_steam(s, k);
    else if (host.find("-lobby.") != std::string::npos && k.lobby_stage == 1) { lobby_hello(s, k); k.lobby_stage = 2; }
    else if (host.find("-lobby.") != std::string::npos) lobby_task(s, k, pt);
    else if (g_trace) fprintf(stderr, "[DWP] %s: type 0x%02x sans réponse\n", host.c_str(), pt[0]);
}

void server_on_data(uint32_t s, Sock& k) {
    if (k.lobby_stage == 0 && name_of(k.peer_ip).find("-lobby.") != std::string::npos) {
        if (k.in.size() < LOBBY_PREAMBLE) return;
        k.in.erase(k.in.begin(), k.in.begin() + LOBBY_PREAMBLE);
        k.lobby_stage = 1;
    }
    for (;;) {
        if (k.in.size() < 4) return;
        uint32_t len = le32(k.in.data());
        if (len == 0) {   // keepalive: answered, the client drops a connection that stays silent for 120 s
            k.in.erase(k.in.begin(), k.in.begin() + 4);
            k.rx.insert(k.rx.end(), 4, 0);
            continue;
        }
        if (k.in.size() < 4 + (size_t)len) return;
        std::vector<uint8_t> f(k.in.begin() + 4, k.in.begin() + 4 + len);
        k.in.erase(k.in.begin(), k.in.begin() + 4 + len);
        on_frame(s, k, std::move(f));
    }
}

}  // namespace

void readiness_witness() { readiness_witness_impl(); }
