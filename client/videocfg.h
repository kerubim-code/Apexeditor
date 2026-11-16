#pragma once

#include <string>
#include <vector>

namespace apex::videocfg {

std::string SavedGamesDir();
std::string LocalDir();
std::string ProfileDir();
std::string SteamCfgDir();
std::string GameDir();
bool        SteamFound();
bool        Elevated();

std::string GameDirFound();
bool        GameFound();

bool        LaunchGame(std::string* detail);

struct LaunchOptionsStatus {
    bool        ok             = false;
    bool        changed        = false;
    bool        alreadyPresent = false;
    bool        steamRunning   = false;
    std::string path;
    std::string value;
    std::string detail;
};

enum LaunchOptionFlags : unsigned {
    kLaunchLetterbox = 1u << 0,
    kLaunchAutoexec  = 1u << 1,
    kLaunchAll       = kLaunchLetterbox | kLaunchAutoexec,
};

LaunchOptionsStatus EnsureLaunchOptions(unsigned flags = kLaunchAll);
LaunchOptionsStatus EnsureAutoexecLaunchOption();
LaunchOptionsStatus UpdateLaunchOptionsFile(const std::string& path,
                                            unsigned flags = kLaunchAll);

struct Report {
    bool                     running = false;
    bool                     finished = false;
    bool                     ok = false;
    int                      total = 0;
    int                      done = 0;
    std::vector<std::string> lines;
};

void   StartInstall();
Report Snapshot();

int    InstalledCount();

void   StartAutoexec();
Report SnapshotAutoexec();
bool   AutoexecInstalled();

}
