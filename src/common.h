// Shared includes and small helpers.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);
std::string Lower(std::string s);
std::string FileName(const std::string& path);
bool ReadTextFile(const std::string& path, std::string& out);

// Activity log shown at the bottom of the window. Every line starts with "HH:MM:SS  ".
// The first two characters of the message pick its colour: "ok" success, "!!" problem,
// ".." detail. Safe to call from any thread.
void Log(const char* fmt, ...);
size_t LogCount();
void ForEachLogLine(const std::function<void(const std::string&)>& fn);
void ClearLog();
void RequestLogScroll();   // scroll the log to its end on the next frame
bool TakeLogScroll();
