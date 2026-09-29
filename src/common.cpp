#include "common.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <mutex>

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string Lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

std::string FileName(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

bool ReadTextFile(const std::string& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return true;
}

// ------------------------------------------------------------------- log ---
static std::mutex               g_logMx;
static std::vector<std::string> g_log;
static std::atomic<bool>        g_logScroll{ false };

void Log(const char* fmt, ...) {
    char msg[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1024];
    snprintf(line, sizeof(line), "%02d:%02d:%02d  %s", st.wHour, st.wMinute, st.wSecond, msg);
    std::lock_guard<std::mutex> lk(g_logMx);
    g_log.push_back(line);
    if (g_log.size() > 800) g_log.erase(g_log.begin(), g_log.begin() + 300);
}

size_t LogCount() {
    std::lock_guard<std::mutex> lk(g_logMx);
    return g_log.size();
}

void ForEachLogLine(const std::function<void(const std::string&)>& fn) {
    std::lock_guard<std::mutex> lk(g_logMx);
    for (const auto& l : g_log) fn(l);
}

void ClearLog() {
    std::lock_guard<std::mutex> lk(g_logMx);
    g_log.clear();
}

void RequestLogScroll() { g_logScroll = true; }
bool TakeLogScroll() { return g_logScroll.exchange(false); }
