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
