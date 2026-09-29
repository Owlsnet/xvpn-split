// The PowerShell engine: app-chooser.ps1 (IN/OUT choices, browser switching, proxy control),
// app-bypass.ps1 (the loopback proxy) and split-tunnel.ps1 (destination exceptions).
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
};
KitState ReadKitState();
std::vector<SavedApp> LoadSavedApps(); // apps.config.json
std::vector<std::string> InstalledBrowsers();

// Runs `script args` hidden on a worker thread and streams its report into the log.
// Only one run at a time; returns false when another one is still going.
bool RunEngine(const char* script, const std::string& args, const std::string& what);
bool EngineBusy();
bool TakeRescanRequest();              // true once after each finished run
