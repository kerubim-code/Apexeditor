#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdio>
#include <string>

namespace apex::settings {

inline std::string LocalAppData() {
    char buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
    return (length > 0 && length < MAX_PATH) ? std::string(buffer, length)
                                              : std::string(".");
}

inline std::string EnsureDir(const std::string& path) {
    CreateDirectoryA(path.c_str(), nullptr);
    return path;
}

inline std::string RootDir() { return EnsureDir(LocalAppData() + "\\APEX"); }

inline std::string AppIniPath() { return RootDir() + "\\app.ini"; }

inline std::string ConfigsDir() { return EnsureDir(RootDir() + "\\configs"); }

inline bool GetBool(const char* key, bool fallback) {
    char value[16] = {};
    GetPrivateProfileStringA("app", key, fallback ? "1" : "0", value,
                             static_cast<DWORD>(sizeof(value)), AppIniPath().c_str());
    return value[0] == '1';
}

inline void SetBool(const char* key, bool value) {
    WritePrivateProfileStringA("app", key, value ? "1" : "0", AppIniPath().c_str());
}

inline int GetInt(const char* key, int fallback) {
    return static_cast<int>(
        GetPrivateProfileIntA("app", key, fallback, AppIniPath().c_str()));
}

inline void SetInt(const char* key, int value) {
    char text[32];
    snprintf(text, sizeof(text), "%d", value);
    WritePrivateProfileStringA("app", key, text, AppIniPath().c_str());
}

inline std::string GetString(const char* key, const char* fallback) {
    char value[260] = {};
    GetPrivateProfileStringA("app", key, fallback, value,
                             static_cast<DWORD>(sizeof(value)), AppIniPath().c_str());
    return value;
}

inline void SetString(const char* key, const std::string& value) {
    WritePrivateProfileStringA("app", key, value.c_str(), AppIniPath().c_str());
}

}
