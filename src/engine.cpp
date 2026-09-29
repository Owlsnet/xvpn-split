#include "engine.h"
#include "resource.h"

#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

static const unsigned kDefaultProxyPort = 49500;

struct EmbeddedScript { int id; const char* name; };
static const EmbeddedScript kScripts[] = {
    { IDR_APP_CHOOSER,  "app-chooser.ps1" },
    { IDR_APP_BYPASS,   "app-bypass.ps1" },
    { IDR_SPLIT_TUNNEL, "split-tunnel.ps1" },
};

const std::string& DataDir() {
    static std::string dir;
    if (dir.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
            dir = Narrow(p) + "\\xvpn";
            CoTaskMemFree(p);
        } else {
            char tmp[MAX_PATH] = { 0 };
            GetTempPathA(MAX_PATH, tmp);
            dir = std::string(tmp) + "xvpn";
        }
        CreateDirectoryW(Widen(dir).c_str(), nullptr);
    }
    return dir;
}

std::string ScriptPath(const char* script) { return DataDir() + "\\" + script; }

std::string PsCommand(const char* script, const char* args) {
    return "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + ScriptPath(script) + "\" " + args;
}

// writes `data` to `path` unless the file already holds exactly that
static bool WriteIfChanged(const std::string& path, const void* data, DWORD size) {
    std::string current;
    if (ReadTextFile(path, current) && current.size() == size && memcmp(current.data(), data, size) == 0) return true;
    std::wstring tmp = Widen(path + ".new");
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, nullptr) && written == size;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), Widen(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool InstallEngine() {
    bool ok = true;
    for (const auto& s : kScripts) {
        HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(s.id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
        const void* data = mem ? LockResource(mem) : nullptr;
        DWORD size = res ? SizeofResource(nullptr, res) : 0;
        if (!data || size == 0) {
            Log("!! %s is missing from this build", s.name);
            ok = false;
            continue;
        }
        if (!WriteIfChanged(ScriptPath(s.name), data, size)) {
            Log("!! could not write %s (error %lu)", ScriptPath(s.name).c_str(), GetLastError());
            ok = false;
        }
    }
    return ok;
}

// ------------------------------------------------------------ state files ---
static unsigned JsonNumber(const std::string& s, const char* key) {
    size_t p = s.find(std::string("\"") + key + "\"");
    if (p == std::string::npos) return 0;
    p = s.find(':', p);
    return (p == std::string::npos) ? 0 : (unsigned)strtoul(s.c_str() + p + 1, nullptr, 10);
}

// reads the JSON string that starts at s[i] == '"'
static std::string JsonString(const std::string& s, size_t i) {
    std::string out;
    for (++i; i < s.size() && s[i] != '"'; ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == 'u' && i + 4 < s.size()) { out += (char)strtol(s.substr(i + 1, 4).c_str(), nullptr, 16); i += 4; }
            else out += (n == 'n') ? '\n' : (n == 't') ? '\t' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

static bool JsonField(const std::string& s, size_t from, size_t to, const char* key, std::string& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k, from);
    if (p == std::string::npos || p >= to) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos || p >= to) return false;
    p = s.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || s[p] != '"') return false;
    out = JsonString(s, p);
    return true;
}

// the pid file can outlive the proxy, and Windows reuses process ids
static bool IsProxyProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    WCHAR buf[MAX_PATH * 2] = { 0 };
    DWORD n = MAX_PATH * 2;
    bool ok = QueryFullProcessImageNameW(h, 0, buf, &n) && Lower(FileName(Narrow(buf))) == "powershell.exe";
    CloseHandle(h);
    return ok;
}

KitState ReadKitState() {
    KitState k;
    k.proxyPort = kDefaultProxyPort;
    std::string s;
    if (ReadTextFile(DataDir() + "\\_bypass.pid", s)) {
        k.proxyPid = JsonNumber(s, "ProcessId");
        unsigned port = JsonNumber(s, "Port");
        if (port) k.proxyPort = port;
        k.proxyUp = k.proxyPid != 0 && IsProxyProcess(k.proxyPid);
    }
    if (ReadTextFile(DataDir() + "\\split-tunnel.state.json", s)) {
        for (size_t pos = s.find("\"Prefix\""); pos != std::string::npos; pos = s.find("\"Prefix\"", pos + 8)) k.exceptions++;
    }
    return k;
}

std::vector<SavedApp> LoadSavedApps() {
    std::vector<SavedApp> apps;
    std::string s;
    if (!ReadTextFile(DataDir() + "\\apps.config.json", s)) return apps;
    // ConvertTo-Json output: every app is one flat {...} object inside "Apps"
    size_t pos = s.find("\"Apps\"");
    while (pos != std::string::npos) {
        size_t ob = s.find('{', pos);
        if (ob == std::string::npos) break;
        size_t cb = s.find('}', ob);
        if (cb == std::string::npos) break;
        SavedApp a;
        std::string mode;
        if (JsonField(s, ob, cb, "Path", a.path) && JsonField(s, ob, cb, "Mode", mode)) {
            a.mode = (mode == "Real") ? "OUT" : "IN";
            apps.push_back(a);
        }
        pos = cb + 1;
    }
    return apps;
}

std::vector<std::string> InstalledBrowsers() {
    static const wchar_t* candidates[] = {
        L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
        L"%LOCALAPPDATA%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
        L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
    };
    std::vector<std::string> found;
    for (const wchar_t* c : candidates) {
        WCHAR buf[MAX_PATH * 2] = { 0 };
        if (!ExpandEnvironmentStringsW(c, buf, MAX_PATH * 2)) continue;
        if (GetFileAttributesW(buf) == INVALID_FILE_ATTRIBUTES) continue;
        std::string p = Narrow(buf);
        bool dup = false;
        for (const auto& f : found) dup = dup || Lower(f) == Lower(p);
        if (!dup) found.push_back(p);
    }
    return found;
}

// ------------------------------------------------------------------- runs ---
static std::atomic<bool> g_busy{ false };
static std::atomic<bool> g_rescan{ false };

bool EngineBusy() { return g_busy; }
bool TakeRescanRequest() { return g_rescan.exchange(false); }

// Turns one line of script output into a log line. The scripts print a banner between two
// "====" rulers, then status lines prefixed "[ OK ]", "[WARN]" or "[FAIL]".
struct OutputFilter {
    bool inBanner = false;
    void Line(std::string l) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        size_t b = l.find_first_not_of(' ');
        if (b == std::string::npos) return;
        l = l.substr(b);
        if (l[0] == '=') { inBanner = !inBanner; return; }
        if (inBanner || l.compare(0, 5, "files") == 0 || l.compare(0, 8, "machine") == 0) return;
        RequestLogScroll();
        auto rest = [&]() {
            size_t p = l.find_first_not_of(' ', 6);
            return p == std::string::npos ? std::string() : l.substr(p);
        };
        if (l.compare(0, 6, "[ OK ]") == 0)                                        Log("ok %s", rest().c_str());
        else if (l.compare(0, 6, "[FAIL]") == 0 || l.compare(0, 6, "[WARN]") == 0) Log("!! %s", rest().c_str());
        else                                                                       Log(".. %s", l.c_str());
    }
};

static void EngineWorker(std::wstring cmd, std::string what) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        Log("!! %s: could not create a pipe", what.c_str());
        g_busy = false;
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                  Widen(DataDir()).c_str(), &si, &pi);
    CloseHandle(wr);
    if (!started) {
        Log("!! %s: could not start PowerShell (error %lu)", what.c_str(), GetLastError());
        CloseHandle(rd);
        g_busy = false;
        return;
    }
    // Poll instead of blocking in ReadFile: a program the script starts (a browser) may
    // inherit the write end of the pipe and would otherwise keep this loop waiting.
    OutputFilter filter;
    std::string acc;
    char buf[2048];
    for (;;) {
        bool exited = WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0;
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, (DWORD)std::min<size_t>(avail, sizeof(buf)), &got, nullptr) || got == 0) break;
            acc.append(buf, got);
            for (size_t nl; (nl = acc.find('\n')) != std::string::npos; acc.erase(0, nl + 1)) filter.Line(acc.substr(0, nl));
        }
        if (exited) break;
        Sleep(60);
    }
    if (!acc.empty()) filter.Line(acc);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code != 0) Log("!! %s: PowerShell exited with code %lu", what.c_str(), code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    g_rescan = true;
    g_busy = false;
    RequestLogScroll();
}

bool RunEngine(const char* script, const std::string& args, const std::string& what) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    WCHAR sys[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive "
                       L"-ExecutionPolicy Bypass -File \"" + Widen(ScriptPath(script)) + L"\" " + Widen(args);
    Log(".. %s", what.c_str());
    RequestLogScroll();
    std::thread(EngineWorker, cmd, what).detach();
    return true;
}
