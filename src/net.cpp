#include "net.h"

#include <iphlpapi.h>
#include <netioapi.h>
#include <tlhelp32.h>
#include <winhttp.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

static std::mutex  g_netMx;
static NetState    g_net;
static std::mutex  g_egressMx;
static EgressState g_egress;

static std::string Ip4ToString(const void* addr) {
    char b[INET_ADDRSTRLEN] = { 0 };
    inet_ntop(AF_INET, addr, b, sizeof(b));
    return b;
}

static std::string InterfaceAlias(DWORD idx) {
    NET_LUID luid{};
    if (ConvertInterfaceIndexToLuid(idx, &luid) != NO_ERROR) return std::string();
    WCHAR alias[IF_MAX_STRING_SIZE + 1] = { 0 };
    if (ConvertInterfaceLuidToAlias(&luid, alias, IF_MAX_STRING_SIZE + 1) != NO_ERROR) return std::string();
    return Narrow(alias);
}

// GetAdaptersAddresses into a buffer that grows until the list fits
static bool GetAdapters(std::vector<BYTE>& buf) {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG size = 16 * 1024;
    for (int tries = 0; tries < 4; ++tries) {
        buf.resize(size);
        ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
        if (rc == NO_ERROR) return true;
        if (rc != ERROR_BUFFER_OVERFLOW) return false;
    }
    return false;
}

static std::string InterfaceIp4(DWORD idx) {
    std::vector<BYTE> buf;
    if (!GetAdapters(buf)) return std::string();
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->IfIndex != idx) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            if (u->Address.lpSockaddr->sa_family == AF_INET)
                return Ip4ToString(&((sockaddr_in*)u->Address.lpSockaddr)->sin_addr);
    }
    return std::string();
}

static bool HasDefaultRoute(DWORD idx) {
    PMIB_IPFORWARD_TABLE2 t = nullptr;
    if (GetIpForwardTable2(AF_INET, &t) != NO_ERROR) return false;
    bool found = false;
    for (ULONG i = 0; i < t->NumEntries && !found; ++i)
        found = t->Table[i].DestinationPrefix.PrefixLength == 0 && t->Table[i].InterfaceIndex == idx;
    FreeMibTable(t);
    return found;
}

static int CountRoutes(DWORD idx) {
    PMIB_IPFORWARD_TABLE2 t = nullptr;
    if (GetIpForwardTable2(AF_INET, &t) != NO_ERROR) return 0;
    int n = 0;
    for (ULONG i = 0; i < t->NumEntries; ++i)
        if (t->Table[i].InterfaceIndex == idx) n++;
    FreeMibTable(t);
    return n;
}

// the interface Windows would use to reach `ip`
static DWORD BestInterface(const char* ip) {
    SOCKADDR_INET dst{}, src{};
    dst.si_family = AF_INET;
    inet_pton(AF_INET, ip, &dst.Ipv4.sin_addr);
    MIB_IPFORWARD_ROW2 row{};
    if (GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &row, &src) != NO_ERROR) return 0;
    return row.InterfaceIndex;
}

void RefreshNetState() {
    NetState s;
    std::vector<BYTE> buf;
    if (GetAdapters(buf)) {
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
            if (!HasDefaultRoute(a->IfIndex)) continue;
            std::string alias = Narrow(a->FriendlyName);
            if (alias.find("Host-Only") != std::string::npos || alias.find("Virtual") != std::string::npos) continue;
            s.nicIf = (int)a->IfIndex;
            s.nicAlias = alias;
            for (auto* u = a->FirstUnicastAddress; u && s.nicIp.empty(); u = u->Next)
                if (u->Address.lpSockaddr->sa_family == AF_INET)
                    s.nicIp = Ip4ToString(&((sockaddr_in*)u->Address.lpSockaddr)->sin_addr);
            for (auto* g = a->FirstGatewayAddress; g && s.nicGw.empty(); g = g->Next)
                if (g->Address.lpSockaddr->sa_family == AF_INET)
                    s.nicGw = Ip4ToString(&((sockaddr_in*)g->Address.lpSockaddr)->sin_addr);
            break;
        }
    }
    // Several public addresses are tried because any single one may be a destination
    // exception that deliberately goes out the physical adapter.
    DWORD tunnel = 0;
    for (const char* probe : { "9.9.9.9", "8.8.8.8", "208.67.222.222", "1.1.1.1" }) {
        DWORD w = BestInterface(probe);
        if (w != 0 && (int)w != s.nicIf) { tunnel = w; break; }
    }
    if (tunnel != 0) {
        s.vpnUp = true;
        s.vpnIf = (int)tunnel;
        s.vpnAlias = InterfaceAlias(tunnel);
        s.vpnIp = InterfaceIp4(tunnel);
        s.vpnRoutes = CountRoutes(tunnel);
    }
    std::lock_guard<std::mutex> lk(g_netMx);
    g_net = s;
}

NetState GetNetState() {
    std::lock_guard<std::mutex> lk(g_netMx);
    return g_net;
}

// ---------------------------------------------------------------- egress ---
// The local address comes from a plain socket to the same host, so it shows which path a
// new connection takes. The public address comes from Cloudflare's trace endpoint.
static void EgressWorker() {
    std::string ip, local;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(443);
        inet_pton(AF_INET, "1.0.0.1", &dst.sin_addr);
        if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
            sockaddr_in me{};
            int len = sizeof(me);
            if (getsockname(s, (sockaddr*)&me, &len) == 0) local = Ip4ToString(&me.sin_addr);
        }
        closesocket(s);
    }
    HINTERNET hs = WinHttpOpen(L"xvpn", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (hs) {
        WinHttpSetTimeouts(hs, 8000, 8000, 8000, 8000);
        HINTERNET hc = WinHttpConnect(hs, L"1.0.0.1", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET hr = hc ? WinHttpOpenRequest(hc, L"GET", L"/cdn-cgi/trace", nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        if (hr) {
            if (WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(hr, nullptr)) {
                std::string body;
                DWORD avail = 0;
                while (body.size() < 8192 && WinHttpQueryDataAvailable(hr, &avail) && avail > 0) {
                    std::string chunk(avail, 0);
                    DWORD got = 0;
                    if (!WinHttpReadData(hr, &chunk[0], avail, &got) || got == 0) break;
                    body.append(chunk, 0, got);
                }
                size_t p = body.find("ip=");
                if (p != std::string::npos) {
                    size_t q = body.find_first_of("\r\n", p);
                    ip = body.substr(p + 3, (q == std::string::npos ? body.size() : q) - p - 3);
                }
            } else {
                Log("!! egress check failed (error %lu)", GetLastError());
            }
            WinHttpCloseHandle(hr);
        }
        if (hc) WinHttpCloseHandle(hc);
        WinHttpCloseHandle(hs);
    }
    {
        std::lock_guard<std::mutex> lk(g_egressMx);
        g_egress.ip = ip;
        g_egress.localIp = local;
        g_egress.busy = false;
    }
    if (!ip.empty()) Log("ok egress %s (from %s)", ip.c_str(), local.c_str());
}

void StartEgressProbe() {
    {
        std::lock_guard<std::mutex> lk(g_egressMx);
        if (g_egress.busy) return;
        g_egress.busy = true;
    }
    std::thread(EgressWorker).detach();
}

EgressState GetEgress() {
    std::lock_guard<std::mutex> lk(g_egressMx);
    return g_egress;
}

// ------------------------------------------------------------------ apps ---
static std::map<DWORD, std::string> ProcessPaths() {
    std::map<DWORD, std::string> paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return paths;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            WCHAR buf[MAX_PATH * 2] = { 0 };
            DWORD n = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(h, 0, buf, &n)) paths[pe.th32ProcessID] = Narrow(buf);
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return paths;
}

std::vector<AppRow> ScanApps(const NetState& net, const std::vector<SavedApp>& saved,
                             const std::vector<std::string>& alwaysShow, unsigned proxyPort) {
    std::map<std::string, std::string> modes;   // lower-case path -> IN / OUT
    for (const auto& s : saved) modes[Lower(s.path)] = s.mode;

    std::vector<AppRow> rows;
    std::map<std::string, size_t> index;        // lower-case path -> row
    auto rowFor = [&](const std::string& path) -> AppRow& {
        std::string key = Lower(path);
        auto f = index.find(key);
        if (f != index.end()) return rows[f->second];
        AppRow a;
        a.path = path;
        a.name = FileName(path);
        auto m = modes.find(key);
        a.mode = (m != modes.end()) ? m->second : "IN";
        rows.push_back(a);
        index[key] = rows.size() - 1;
        return rows.back();
    };

    std::map<DWORD, std::string> paths = ProcessPaths();
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER) {
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            auto* t = (MIB_TCPTABLE_OWNER_PID*)buf.data();
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& r = t->table[i];
                if (r.dwState != MIB_TCP_STATE_ESTAB) continue;
                auto it = paths.find(r.dwOwningPid);
                if (it == paths.end()) continue;
                std::string local = Ip4ToString(&r.dwLocalAddr);
                std::string remote = Ip4ToString(&r.dwRemoteAddr);
                unsigned rport = ntohs((u_short)r.dwRemotePort);
                AppRow& a = rowFor(it->second);
                if (remote == "127.0.0.1" && rport == proxyPort)   a.proxy++;
                else if (!net.nicIp.empty() && local == net.nicIp) a.real++;
                else if (!net.vpnIp.empty() && local == net.vpnIp) a.vpn++;
            }
        }
    }
    for (const auto& s : saved) rowFor(s.path);
    for (const auto& p : alwaysShow) rowFor(p);

    for (auto& a : rows) {
        int out = a.real + a.proxy;
        if (out > 0 && a.vpn > 0) a.live = "MIXED";
        else if (a.proxy > 0)     a.live = "OUT proxy";
        else if (a.real > 0)      a.live = "OUT real";
        else if (a.vpn > 0)       a.live = "IN";
        else                      a.live = "idle";
    }
    std::sort(rows.begin(), rows.end(), [](const AppRow& x, const AppRow& y) {
        return _stricmp(x.name.c_str(), y.name.c_str()) < 0;
    });
    return rows;
}
