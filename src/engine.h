// The PowerShell engine: app-chooser.ps1 (IN/OUT choices, browser switching, proxy control),
// app-bypass.ps1 (the loopback proxy), split-tunnel.ps1 (destination exceptions) and
// hotspot.ps1 (a proxy that lets devices on the Mobile Hotspot use the VPN).
// The scripts are embedded in the exe and unpacked to DataDir() at start, where they also
// keep their state files.
#pragma once
#include "common.h"
#include "net.h"

const std::string& DataDir();          // %LOCALAPPDATA%\xvpn
bool InstallEngine();                  // unpacks the scripts; logs and returns false on failure
std::string ScriptPath(const char* script);
std::string PsCommand(const char* script, const char* args);   // ready-to-paste command line

struct KitState {
    bool     proxyUp = false;
    unsigned proxyPid = 0;
    unsigned proxyPort = 0;
    int      exceptions = 0;          // destination exceptions installed by split-tunnel.ps1
    bool     hotspotShared = false;   // hotspot.ps1 -Start is in effect
    bool     hotspotWatcher = false;  // its proxy process is running
    // last status written by the hotspot proxy (the window cannot ask Windows itself)
    bool     hotspotOn = false, hotspotLive = false;
    int      hotspotClients = 0, hotspotMaxClients = 0;
    unsigned hotspotPort = 0, hotspotRequests = 0;
    std::string hotspotTunnel, hotspotSsid, hotspotProblem, hotspotHost, hotspotBand;
};
KitState ReadKitState();
std::vector<SavedApp> LoadSavedApps(); // apps.config.json
std::vector<std::string> InstalledBrowsers();
std::vector<std::string> InstalledExceptionTargets();   // the addresses and hostnames behind the exceptions

// hotspot name and password, remembered between runs (encrypted for the current Windows user)
struct HotspotSettings { std::string name, pass; };
bool LoadHotspotSettings(HotspotSettings& out);
bool SaveHotspotSettings(const HotspotSettings& in);

// Runs `script args` hidden on a worker thread and streams its report into the log.
// Only one run at a time; returns false when another one is still going.
bool RunEngine(const char* script, const std::string& args, const std::string& what);
// The same, elevated: Windows asks the user for permission first. The script's output comes
// back through the -OutFile it is given, since an elevated process cannot share a pipe.
bool RunEngineElevated(const char* script, const std::string& args, const std::string& what);
bool EngineBusy();
bool TakeRescanRequest();              // true once after each finished run
