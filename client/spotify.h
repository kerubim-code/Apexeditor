#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apex::spotify {

struct NowPlaying {
    bool        available = false;
    bool        playing   = false;
    std::string title;
    std::string artist;
    std::string album;
    long long   posMs = 0;
    long long   durMs = 0;
    unsigned    artSerial = 0;
    bool        hasArt = false;
};

struct LyricLine {
    long long   tMs = -1;
    std::string text;
};

enum class Cmd { PlayPause, Next, Prev };

void Start();
void Stop();

bool Snapshot(NowPlaying& out);

bool CopyArt(unsigned serial, std::vector<unsigned char>& rgba, int& w, int& h);

void Post(Cmd c);
void Seek(long long ms);

void RequestLyrics(const std::string& artist, const std::string& track);
bool LyricsSnapshot(std::vector<LyricLine>& out, std::string& status,
                    std::string& key);

}
