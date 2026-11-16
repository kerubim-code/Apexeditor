#define _CRT_SECURE_NO_WARNINGS

#include "videocfg.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <urlmon.h>

#include <cwchar>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#pragma comment(lib, "urlmon.lib")

namespace apex::videocfg {

namespace {

constexpr const char* kRepo =
    "https://raw.githubusercontent.com/kerubim-code/APEX-SETTINGS/main";

constexpr const char* kSteamAppId = "1172470";

constexpr const char* kMatLaunchArg  = "+mat_letterbox_aspect_min 1.0";
constexpr const char* kExecLaunchArg = "+exec autoexec.cfg";

constexpr const char* kGameExes[] = {"r5apex_dx12.exe", "r5apex.exe"};

std::mutex g_mutex;
Report     g_settings;
Report     g_autoexec;

std::string UserProfile() {
    char buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA("USERPROFILE", buffer, MAX_PATH);
    return (length > 0 && length < MAX_PATH) ? std::string(buffer, length)
                                              : std::string("C:\\\\");
}

bool PathExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool FileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDir(const std::string& dir) {
    std::string built;
    built.reserve(dir.size());
    for (char c : dir) {
        built += c;
        if ((c == '\\' || c == '/') && built.size() > 3)
            CreateDirectoryA(built.c_str(), nullptr);
    }
    CreateDirectoryA(dir.c_str(), nullptr);
}

std::string BaseName(const std::string& rel) {
    const auto slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

void Push(Report& report, const std::string& line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    report.lines.push_back(line);
}

bool Download(const std::string& url, const std::string& dest) {

    const std::string tmp = dest + ".apexdl";
    DeleteFileA(tmp.c_str());

    const HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(), tmp.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        DeleteFileA(tmp.c_str());
        return false;
    }

    DeleteFileA(dest.c_str());
    if (!MoveFileA(tmp.c_str(), dest.c_str())) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

struct Job {
    std::string url;
    std::string dest;
    std::string name;
};

void RunJobs(Report& report, std::vector<Job>& jobs, const std::string& skipLine) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        report.total   = static_cast<int>(jobs.size());
        report.done    = 0;
        report.ok      = true;
        report.lines.clear();
        report.finished = false;
    }

    if (!skipLine.empty())
        Push(report, skipLine);

    int failed = 0;
    for (const auto& job : jobs) {
        const bool ok = Download(job.url, job.dest);
        if (!ok) ++failed;
        Push(report, std::string(ok ? "ok     " : "failed ") + job.name);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ++report.done;
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    report.running  = false;
    report.finished = true;
    report.ok       = (failed == 0);
    report.done     = static_cast<int>(jobs.size());
}

void AddJob(std::vector<Job>& jobs, const char* rel, const std::string& dir) {
    jobs.push_back({std::string(kRepo) + "/" + rel, dir + "\\" + BaseName(rel),
                    BaseName(rel)});
}

void SettingsWorker() {
    const std::string local   = LocalDir();
    const std::string profile = ProfileDir();

    EnsureDir(SavedGamesDir());
    EnsureDir(local);
    EnsureDir(profile);

    std::vector<Job> jobs;
    AddJob(jobs, "local/videoconfig.txt", local);
    AddJob(jobs, "local/voice_volumes.dat", local);
    AddJob(jobs, "profile/steam_autocloud.vdf", profile);

    RunJobs(g_settings, jobs, std::string());
}

void AutoexecWorker() {
    const bool wantSteam = SteamFound();
    const bool gotSteam  = wantSteam && Elevated();

    if (gotSteam)
        EnsureDir(SteamCfgDir());

    std::vector<Job> jobs;
    if (gotSteam)
        AddJob(jobs, "autoexec.cfg", SteamCfgDir());

    std::string skip;
    if (!wantSteam)
        skip = "failed steam not found  -  autoexec.cfg cannot be installed";
    else if (!gotSteam)
        skip = "skip  autoexec.cfg  -  restart APEX as administrator";

    RunJobs(g_autoexec, jobs, skip);

    if (!wantSteam || !gotSteam) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_autoexec.ok = false;
    }
}

std::string RegString(HKEY key, const char* valueName) {
    char     buffer[MAX_PATH] = {};
    DWORD    size = sizeof(buffer) - 1;
    DWORD    type = 0;
    if (RegQueryValueExA(key, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
        return std::string();
    buffer[MAX_PATH - 1] = '\0';
    return std::string(buffer);
}

std::string VdfPathValue(const std::string& line) {
    const auto keyAt = line.find("\"path\"");
    if (keyAt == std::string::npos) return std::string();

    const auto q1 = line.find('"', keyAt + 6);
    if (q1 == std::string::npos) return std::string();
    const auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();

    std::string raw = line.substr(q1 + 1, q2 - q1 - 1);
    std::string clean;
    clean.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size())
            clean += raw[++i];
        else
            clean += raw[i];
    }
    return clean;
}

std::vector<std::string> SteamRoots() {
    std::vector<std::string> roots;

    auto add = [&roots](std::string root) {
        while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
            root.pop_back();
        if (root.empty()) return;

        for (char& c : root)
            if (c == '/') c = '\\';
        for (const auto& existing : roots)
            if (existing == root) return;
        roots.push_back(root);
    };

    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        add(RegString(key, "SteamPath"));
        RegCloseKey(key);
        key = nullptr;
    }
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        add(RegString(key, "InstallPath"));
        RegCloseKey(key);
        key = nullptr;
    }
    add("C:\\Program Files (x86)\\Steam");
    add("C:\\Program Files\\Steam");

    std::vector<std::string> extra;
    for (const auto& root : roots) {
        std::ifstream in(root + "\\steamapps\\libraryfolders.vdf");
        if (!in) continue;
        std::string line;
        while (std::getline(in, line)) {
            std::string path = VdfPathValue(line);
            if (!path.empty()) extra.push_back(path);
        }
    }
    for (const auto& lib : extra)
        add(lib);

    return roots;
}

std::string GameExeIn(const std::string& folder) {
    for (const char* name : kGameExes) {
        const std::string candidate = folder + "\\" + name;
        if (FileExists(candidate)) return candidate;
    }
    return std::string();
}

bool SteamRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    bool running = false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

std::string TrimText(const std::string& text) {
    size_t first = 0;
    size_t last  = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t' ||
                            text[first] == '\r' || text[first] == '\n'))
        ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' ||
                            text[last - 1] == '\r' || text[last - 1] == '\n'))
        --last;
    return text.substr(first, last - first);
}

std::string LowerText(const std::string& text) {
    std::string out = text;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool ContainsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return LowerText(haystack).find(LowerText(needle)) != std::string::npos;
}

std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return std::string();
    std::string text;
    text.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return text;
}

std::string UnescapeVdf(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            out += value[i + 1];
            ++i;
        } else {
            out += value[i];
        }
    }
    return out;
}

struct TextLine {
    size_t start = 0;
    size_t end   = 0;
    size_t next  = 0;
};

std::vector<TextLine> SplitTextLines(const std::string& text) {
    std::vector<TextLine> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t newline = text.find('\n', pos);
        size_t       end     = newline == std::string::npos ? text.size() : newline;
        const size_t next    = newline == std::string::npos ? text.size() : newline + 1;
        if (end > pos && text[end - 1] == '\r') --end;
        lines.push_back(TextLine{pos, end, next});
        if (newline == std::string::npos) break;
        pos = newline + 1;
    }
    return lines;
}

std::string LineText(const std::string& text, const TextLine& line) {
    return text.substr(line.start, line.end - line.start);
}

int BraceDelta(const std::string& line) {
    int  depth  = 0;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '\\' && i + 1 < line.size()) {
                ++i;
                continue;
            }
            if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
        }
    }
    return depth;
}

bool IsOpenBraceLine(const std::string& line) {
    return TrimText(line) == "{";
}

bool ParseQuoted(const std::string& line, size_t& pos, std::string* value,
                 size_t* openPos, size_t* closePos) {
    while (pos < line.size() &&
           (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r' ||
            line[pos] == '\n'))
        ++pos;
    if (pos >= line.size() || line[pos] != '"') return false;

    const size_t open = pos++;
    std::string  out;
    while (pos < line.size()) {
        const char c = line[pos];
        if (c == '\\' && pos + 1 < line.size()) {
            out += c;
            out += line[pos + 1];
            pos += 2;
            continue;
        }
        if (c == '"') {
            if (value) *value = out;
            if (openPos) *openPos = open;
            if (closePos) *closePos = pos;
            ++pos;
            return true;
        }
        out += c;
        ++pos;
    }
    return false;
}

struct ParsedLine {
    std::string key;
    std::string value;
    bool        hasValue   = false;
    size_t      valueOpen  = 0;
    size_t      valueClose = 0;
};

bool ParseVdfLine(const std::string& line, ParsedLine* out) {
    size_t      pos = 0;
    std::string key;
    if (!ParseQuoted(line, pos, &key, nullptr, nullptr)) return false;

    out->key      = key;
    out->hasValue = false;
    if (ParseQuoted(line, pos, &out->value, &out->valueOpen, &out->valueClose))
        out->hasValue = true;
    return true;
}

struct VdfBlock {
    size_t open  = 0;
    size_t close = 0;
};

size_t NextContentLine(const std::vector<TextLine>& lines,
                       const std::string& text, size_t from, size_t to) {
    while (from < to) {
        const std::string trimmed = TrimText(LineText(text, lines[from]));
        if (!trimmed.empty() && trimmed.compare(0, 2, "//") != 0) return from;
        ++from;
    }
    return to;
}

size_t FindMatchingBrace(const std::vector<TextLine>& lines,
                         const std::string& text, size_t open, size_t limit) {
    if (open >= limit) return std::string::npos;
    int depth = 0;
    for (size_t i = open; i < limit; ++i) {
        depth += BraceDelta(LineText(text, lines[i]));
        if (depth == 0) return i;
    }
    return std::string::npos;
}

bool FindChildBlock(const std::vector<TextLine>& lines, const std::string& text,
                    size_t from, size_t to, const std::string& key,
                    VdfBlock* out) {
    size_t i = from;
    while (i < to) {
        ParsedLine parsed;
        if (ParseVdfLine(LineText(text, lines[i]), &parsed) && !parsed.hasValue) {
            const size_t next = NextContentLine(lines, text, i + 1, to);
            if (next < to && IsOpenBraceLine(LineText(text, lines[next]))) {
                const size_t close = FindMatchingBrace(lines, text, next, to);
                if (close == std::string::npos) return false;
                if (parsed.key == key) {
                    out->open  = next;
                    out->close = close;
                    return true;
                }
                i = close + 1;
                continue;
            }
        }
        ++i;
    }
    return false;
}

bool ReadDirectValue(const std::vector<TextLine>& lines, const std::string& text,
                     const VdfBlock& block, const char* key, std::string* value) {
    int depth = 0;
    for (size_t i = block.open + 1; i < block.close; ++i) {
        const std::string line = LineText(text, lines[i]);
        if (depth == 0) {
            ParsedLine parsed;
            if (ParseVdfLine(line, &parsed) && parsed.hasValue &&
                parsed.key == key) {
                if (value) *value = UnescapeVdf(parsed.value);
                return true;
            }
        }
        depth += BraceDelta(line);
    }
    return false;
}

std::string ActiveSteamId(const std::string& root) {
    const std::string text = ReadWholeFile(root + "\\config\\loginusers.vdf");
    if (text.empty()) return std::string();

    const std::vector<TextLine> lines = SplitTextLines(text);
    VdfBlock users;
    if (!FindChildBlock(lines, text, 0, lines.size(), "users", &users))
        return std::string();

    std::string bestId;
    long long   bestTime = -1;
    bool        bestAuto = false;

    size_t i = users.open + 1;
    while (i < users.close) {
        ParsedLine parsed;
        if (ParseVdfLine(LineText(text, lines[i]), &parsed) && !parsed.hasValue) {
            const size_t next =
                NextContentLine(lines, text, i + 1, users.close);
            if (next < users.close &&
                IsOpenBraceLine(LineText(text, lines[next]))) {
                const size_t close =
                    FindMatchingBrace(lines, text, next, users.close);
                if (close == std::string::npos) break;

                VdfBlock account;
                account.open  = next;
                account.close = close;

                std::string autoText;
                std::string timeText;
                ReadDirectValue(lines, text, account, "AutoLogin", &autoText);
                ReadDirectValue(lines, text, account, "timestamp", &timeText);

                const long long stamp =
                    timeText.empty() ? 0 : std::strtoll(timeText.c_str(), nullptr, 10);
                const bool autoLogin = autoText == "1";

                if (stamp > bestTime ||
                    (stamp == bestTime && autoLogin && !bestAuto)) {
                    bestTime = stamp;
                    bestAuto = autoLogin;
                    bestId   = parsed.key;
                }

                i = close + 1;
                continue;
            }
        }
        ++i;
    }
    return bestId;
}

std::string NewestLocalConfig(const std::string& root) {
    std::string      best;
    ULONGLONG        bestTime = 0;
    WIN32_FIND_DATAA findData{};
    HANDLE handle = FindFirstFileA((root + "\\userdata\\*").c_str(), &findData);
    if (handle == INVALID_HANDLE_VALUE) return best;

    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (std::strcmp(findData.cFileName, ".") == 0 ||
            std::strcmp(findData.cFileName, "..") == 0)
            continue;

        const std::string path = root + "\\userdata\\" + findData.cFileName +
                                 "\\config\\localconfig.vdf";
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info))
            continue;

        ULARGE_INTEGER stamp{};
        stamp.LowPart  = info.ftLastWriteTime.dwLowDateTime;
        stamp.HighPart = info.ftLastWriteTime.dwHighDateTime;
        if (stamp.QuadPart > bestTime) {
            bestTime = stamp.QuadPart;
            best     = path;
        }
    } while (FindNextFileA(handle, &findData));
    FindClose(handle);
    return best;
}

std::string FindSteamLocalConfig() {
    for (const auto& root : SteamRoots()) {
        const std::string id = ActiveSteamId(root);
        if (!id.empty()) {
            const std::string path =
                root + "\\userdata\\" + id + "\\config\\localconfig.vdf";
            if (FileExists(path)) return path;
        }

        const std::string newest = NewestLocalConfig(root);
        if (!newest.empty()) return newest;
    }
    return std::string();
}

LaunchOptionsStatus ApplyLaunchOptionsFile(const std::string& path, bool write,
                                          unsigned flags) {
    LaunchOptionsStatus status;
    status.path = path;

    std::string text = ReadWholeFile(path);
    if (text.empty()) {
        status.detail = "steam localconfig not found";
        return status;
    }

    const std::vector<TextLine> lines = SplitTextLines(text);
    VdfBlock user;
    VdfBlock software;
    VdfBlock valve;
    VdfBlock steam;
    VdfBlock apps;
    VdfBlock app;

    if (!FindChildBlock(lines, text, 0, lines.size(), "UserLocalConfigStore",
                        &user) ||
        !FindChildBlock(lines, text, user.open + 1, user.close, "Software",
                        &software) ||
        !FindChildBlock(lines, text, software.open + 1, software.close, "Valve",
                        &valve) ||
        !FindChildBlock(lines, text, valve.open + 1, valve.close, "Steam",
                        &steam) ||
        !FindChildBlock(lines, text, steam.open + 1, steam.close, "apps",
                        &apps) ||
        !FindChildBlock(lines, text, apps.open + 1, apps.close, kSteamAppId,
                        &app)) {
        status.detail = "apex not found in steam config";
        return status;
    }

    size_t     optionLine = std::string::npos;
    ParsedLine option;
    int        depth = 0;
    for (size_t i = app.open + 1; i < app.close; ++i) {
        const std::string line = LineText(text, lines[i]);
        if (depth == 0) {
            ParsedLine parsed;
            if (ParseVdfLine(line, &parsed) && parsed.key == "LaunchOptions") {
                optionLine = i;
                option     = parsed;
                break;
            }
        }
        depth += BraceDelta(line);
    }

    const bool        wantMat  = (flags & kLaunchLetterbox) != 0;
    const bool        wantExec = (flags & kLaunchAutoexec) != 0;
    const std::string rawValue = option.hasValue ? option.value : std::string();
    const bool        hasMat   = !wantMat || ContainsNoCase(rawValue, kMatLaunchArg);
    const bool        hasExec  = !wantExec || ContainsNoCase(rawValue, kExecLaunchArg);

    status.value = UnescapeVdf(rawValue);
    if (hasMat && hasExec) {
        status.ok             = true;
        status.alreadyPresent = true;
        status.detail         = "already set";
        return status;
    }

    if (!write) {
        status.detail = "steam running";
        return status;
    }

    std::string nextValue = rawValue;
    auto appendArg = [&nextValue](const char* arg) {
        if (!nextValue.empty() && nextValue.back() != ' ' &&
            nextValue.back() != '\t')
            nextValue += ' ';
        nextValue += arg;
    };
    if (!hasMat) appendArg(kMatLaunchArg);
    if (!hasExec) appendArg(kExecLaunchArg);

    if (optionLine != std::string::npos) {
        if (option.hasValue) {
            text.replace(lines[optionLine].start + option.valueOpen + 1,
                         option.valueClose - option.valueOpen - 1, nextValue);
        } else {
            std::string indent;
            for (char c : LineText(text, lines[optionLine])) {
                if (c == ' ' || c == '\t')
                    indent += c;
                else
                    break;
            }
            text.replace(lines[optionLine].start,
                         lines[optionLine].end - lines[optionLine].start,
                         indent + "\"LaunchOptions\"\t\t\"" + nextValue + "\"");
        }
    } else {
        std::string indent;
        for (char c : LineText(text, lines[app.close])) {
            if (c == ' ' || c == '\t')
                indent += c;
            else
                break;
        }
        indent += '\t';
        const std::string eol =
            text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        text.insert(lines[app.close].start,
                    indent + "\"LaunchOptions\"\t\t\"" + nextValue + "\"" + eol);
    }

    const std::string backup = path + ".apex.bak";
    if (!CopyFileA(path.c_str(), backup.c_str(), FALSE)) {
        status.detail = "could not back up steam config";
        return status;
    }

    const std::string temp = path + ".apextmp";
    std::ofstream out(temp.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        DeleteFileA(temp.c_str());
        status.detail = "could not write steam config";
        return status;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        DeleteFileA(temp.c_str());
        status.detail = "could not write steam config";
        return status;
    }

    if (!MoveFileExA(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temp.c_str());
        status.detail = "could not save steam config";
        return status;
    }

    status.ok      = true;
    status.changed = true;
    status.value   = UnescapeVdf(nextValue);
    status.detail  = "launch options updated";
    return status;
}

std::string FindGameFolder() {
    for (const auto& root : SteamRoots()) {
        const std::string candidate = root + "\\steamapps\\common\\Apex Legends";
        if (!GameExeIn(candidate).empty())
            return candidate;
    }

    const char* eaRoots[] = {"C:\\Program Files\\Electronic Arts\\Apex Legends",
                             "C:\\Program Files (x86)\\Electronic Arts\\Apex Legends",
                             "D:\\Electronic Arts\\Apex Legends"};
    for (const char* candidate : eaRoots)
        if (!GameExeIn(candidate).empty())
            return std::string(candidate);
    return std::string();
}

}

std::string SavedGamesDir() { return UserProfile() + "\\Saved Games\\Respawn\\Apex"; }
std::string LocalDir() { return SavedGamesDir() + "\\local"; }
std::string ProfileDir() { return SavedGamesDir() + "\\profile"; }

std::string SteamCfgDir() {
    const std::string folder = GameDirFound();
    if (!folder.empty()) return folder + "\\cfg";
    return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Apex Legends\\cfg";
}

std::string GameDir() {
    const std::string found = GameDirFound();
    return found.empty() ? SteamCfgDir() : found + "\\cfg";
}

bool SteamFound() { return PathExists(SteamCfgDir()); }

std::string GameDirFound() { return FindGameFolder(); }

bool GameFound() { return !GameDirFound().empty(); }

bool LaunchGame(std::string* detail) {
    const std::string folder = GameDirFound();
    const std::string exe    = folder.empty() ? std::string() : GameExeIn(folder);

    if (SteamRunning()) {
        const std::string protocol = std::string("steam://run/") + kSteamAppId;
        const HINSTANCE   result =
            ShellExecuteA(nullptr, "open", protocol.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            if (detail) *detail = "launching through Steam  -  " + protocol;
            return true;
        }
    }

    if (!exe.empty()) {
        const HINSTANCE result = ShellExecuteA(nullptr, "open", exe.c_str(),
                                               nullptr, folder.c_str(),
                                               SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            if (detail) *detail = "launching " + exe + "  -  " + folder;
            return true;
        }
    }

    const std::string protocol = std::string("steam://run/") + kSteamAppId;
    const HINSTANCE   result =
        ShellExecuteA(nullptr, "open", protocol.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) > 32) {
        if (detail) *detail = "asking Steam to launch Apex Legends  -  " + protocol;
        return true;
    }

    if (detail)
        *detail = "Apex Legends was not found on this PC";
    return false;
}

bool Elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD           size = 0;
    const BOOL ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok == TRUE && elevation.TokenIsElevated != 0;
}

void StartInstall() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_settings.running) return;
        g_settings          = Report{};
        g_settings.running  = true;
    }
    std::thread([] { SettingsWorker(); }).detach();
}

Report Snapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_settings;
}

int InstalledCount() {
    int count = 0;
    if (FileExists(LocalDir() + "\\videoconfig.txt")) ++count;
    if (FileExists(LocalDir() + "\\voice_volumes.dat")) ++count;
    if (FileExists(ProfileDir() + "\\steam_autocloud.vdf")) ++count;
    return count;
}

void StartAutoexec() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_autoexec.running) return;
        g_autoexec          = Report{};
        g_autoexec.running  = true;
    }
    std::thread([] { AutoexecWorker(); }).detach();
}

Report SnapshotAutoexec() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_autoexec;
}

bool AutoexecInstalled() {
    return FileExists(SteamCfgDir() + "\\autoexec.cfg");
}

LaunchOptionsStatus UpdateLaunchOptionsFile(const std::string& path, unsigned flags) {
    return ApplyLaunchOptionsFile(path, true, flags);
}

LaunchOptionsStatus EnsureLaunchOptions(unsigned flags) {
    LaunchOptionsStatus status;
    const std::string   path = FindSteamLocalConfig();
    if (path.empty()) {
        status.detail = "steam localconfig not found";
        return status;
    }

    const bool running = SteamRunning();
    status             = ApplyLaunchOptionsFile(path, !running, flags);
    status.steamRunning = running;
    if (!status.ok && running && status.detail == "steam running")
        status.detail = "close steam, then click again";
    return status;
}

LaunchOptionsStatus EnsureAutoexecLaunchOption() {
    return EnsureLaunchOptions(kLaunchAutoexec);
}

}
