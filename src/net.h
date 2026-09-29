// What the network looks like right now: physical adapter, VPN tunnel, egress address,
// and which side of the tunnel each application's connections are on.
#pragma once
#include "common.h"

struct NetState {
    bool        vpnUp = false;
    int         vpnIf = 0;
    int         vpnRoutes = 0;
    std::string vpnAlias, vpnIp;
    int         nicIf = 0;
    std::string nicAlias, nicIp, nicGw;
};

// Re-reads adapters and routes. The physical adapter is the first adapter that is up and
// owns a default route; the tunnel is whichever interface Windows picks for public
// addresses when that is not the physical adapter.
void RefreshNetState();
NetState GetNetState();

struct EgressState {
    std::string ip;        // public address seen by the remote side
    std::string localIp;   // local address the connection left from
    bool        busy = false;
};

// Measures the egress address on a background thread; read the result with GetEgress().
void StartEgressProbe();
EgressState GetEgress();

struct AppRow {
    std::string name, path;
    std::string mode;          // IN or OUT, as saved by the engine
    std::string live;          // IN, OUT proxy, OUT real, MIXED or idle
    int vpn = 0, real = 0, proxy = 0;
};

struct SavedApp {
    std::string path;
    std::string mode;          // IN or OUT
};

// Every process with established TCP connections, classified by the local address of each
// connection (tunnel / physical adapter) or by talking to the bypass proxy. `alwaysShow`
// paths are listed even without connections.
std::vector<AppRow> ScanApps(const NetState& net, const std::vector<SavedApp>& saved,
                             const std::vector<std::string>& alwaysShow, unsigned proxyPort);
