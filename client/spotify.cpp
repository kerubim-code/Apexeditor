#include "spotify.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <objbase.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")

namespace apex::spotify {
namespace {

namespace wmc    = winrt::Windows::Media::Control;
namespace streams = winrt::Windows::Storage::Streams;
using namespace std::chrono;

struct State {
    bool        available = false;
    bool        playing   = false;
    std::string title, artist, album;
    long long   durMs = 0;
    long long   posBase100 = 0;
    steady_clock::time_point posWall{};
    double      rate = 1.0;
    long long   posLast100 = 0;
    bool        posInit    = false;
    unsigned    artSerial = 1;
    bool        hasArt = false;
    std::vector<unsigned char> artRgba;
    int         artW = 0, artH = 0;
};
std::mutex g_mtx;
State      g_state;

std::mutex   g_cmdMtx;
std::deque<Cmd> g_cmds;
std::deque<long long> g_seeks;
std::mutex   g_pollMtx;
std::condition_variable g_pollCv;
bool         g_wake = false;

std::atomic<bool> g_running{false};
std::thread  g_pollThread;

struct LyricState {
    std::string key;
    std::string status;
    std::vector<LyricLine> lines;
};
std::mutex g_lyrMtx;
LyricState g_lyr;
std::string g_reqKey;
unsigned long long g_reqId = 0;

struct Job {
    unsigned long long id = 0;
    std::string artist, track, key;
};
std::mutex g_jobMtx;
std::condition_variable g_jobCv;
bool g_jobPending = false;
Job  g_job;
std::thread g_lyrThread;

std::string TrackKey(const std::string& artist, const std::string& track) {
    return artist + "|" + track;
}

bool FetchArt(winrt::Windows::Storage::Streams::IRandomAccessStreamReference const& thumb,
              std::vector<unsigned char>& rgba, int& w, int& h) {
    if (!thumb)
        return false;
    try {
        auto stream = thumb.OpenReadAsync().get();
        if (!stream)
            return false;
        const uint64_t size = stream.Size();
        if (size == 0 || size > 12ull * 1024 * 1024)
            return false;

        streams::DataReader reader{stream};
        const uint32_t len = reader.LoadAsync(static_cast<uint32_t>(size)).get();
        if (len == 0)
            return false;
        std::vector<unsigned char> bytes(len);
        reader.ReadBytes(bytes);
        stream.Close();

        IStream* mem = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
        if (mem == nullptr)
            return false;

        bool ok = false;
        IWICImagingFactory* fac = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) && fac) {
            IWICBitmapDecoder* dec = nullptr;
            if (SUCCEEDED(fac->CreateDecoderFromStream(mem, nullptr,
                                                       WICDecodeMetadataCacheOnDemand, &dec)) &&
                dec) {
                IWICBitmapFrameDecode* frame = nullptr;
                if (SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
                    IWICFormatConverter* conv = nullptr;
                    if (SUCCEEDED(fac->CreateFormatConverter(&conv)) && conv) {
                        if (SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                                       WICBitmapPaletteTypeCustom))) {
                            UINT bw = 0, bh = 0;
                            conv->GetSize(&bw, &bh);
                            if (bw > 0 && bh > 0 && bw <= 4096 && bh <= 4096) {
                                rgba.resize(static_cast<size_t>(bw) * bh * 4);
                                if (SUCCEEDED(conv->CopyPixels(nullptr, bw * 4,
                                                               static_cast<DWORD>(rgba.size()),
                                                               rgba.data()))) {
                                    w = static_cast<int>(bw);
                                    h = static_cast<int>(bh);
                                    ok = true;
                                }
                            }
                        }
                        conv->Release();
                    }
                    frame->Release();
                }
                dec->Release();
            }
            fac->Release();
        }
        mem->Release();
        return ok;
    } catch (...) {
        return false;
    }
}

void PollOnce(wmc::GlobalSystemMediaTransportControlsSessionManager const& mgr) {
    auto session = mgr ? mgr.GetCurrentSession() : nullptr;

    std::deque<Cmd> cmds;
    std::deque<long long> seeks;
    {
        std::lock_guard<std::mutex> lk(g_cmdMtx);
        cmds.swap(g_cmds);
        seeks.swap(g_seeks);
    }
    if (session) {
        for (Cmd c : cmds) {
            try {
                switch (c) {
                case Cmd::PlayPause:
                    session.TryTogglePlayPauseAsync().get();
                    break;
                case Cmd::Next:
                    session.TrySkipNextAsync().get();
                    break;
                case Cmd::Prev:
                    session.TrySkipPreviousAsync().get();
                    break;
                }
            } catch (...) {
            }
        }
        if (!seeks.empty()) {
            try {
                session.TryChangePlaybackPositionAsync(seeks.back() * 10000LL).get();
            } catch (...) {
            }
        }
    }

    if (!session) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_state.available = false;
        g_state.playing   = false;
        return;
    }

    try {
        auto props = session.TryGetMediaPropertiesAsync().get();
        auto pbi   = session.GetPlaybackInfo();
        auto tl    = session.GetTimelineProperties();

        std::string title  = winrt::to_string(props.Title());
        std::string artist = winrt::to_string(props.Artist());
        std::string album  = winrt::to_string(props.AlbumTitle());
        const bool playing =
            pbi.PlaybackStatus() ==
            wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing ||
            pbi.PlaybackStatus() ==
            wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing;

        long long durMs = tl.MaxSeekTime().count() / 10000LL;
        const double rate = 1.0;
        const long long reported = tl.Position().count();
        const steady_clock::time_point now = steady_clock::now();

        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);

            long long predicted = g_state.posBase100;
            if (g_state.available && g_state.playing) {
                const long long el =
                    duration_cast<milliseconds>(now - g_state.posWall).count();
                predicted += static_cast<long long>(
                                 static_cast<double>(el) * g_state.rate) *
                             10000LL;
            }
            const bool fresh = !g_state.posInit || reported != g_state.posLast100;
            const bool flipped =
                g_state.posInit && (g_state.playing != playing);
            if (fresh) {
                const long long delta = reported - predicted;
                const long long kSnap = 1500LL * 10000LL;
                const long long kEase =  300LL * 10000LL;
                if (!g_state.posInit || flipped || delta > kSnap || delta < -kSnap)
                    predicted = reported;
                else if (delta > kEase || delta < -kEase)
                    predicted += delta / 4;
                g_state.posLast100 = reported;
                g_state.posInit    = true;
            }
            g_state.posBase100 = predicted;
            g_state.posWall    = now;

            changed = !g_state.available || title != g_state.title ||
                      artist != g_state.artist || album != g_state.album;
            g_state.available = true;
            g_state.playing   = playing;
            g_state.title     = std::move(title);
            g_state.artist    = std::move(artist);
            g_state.album     = std::move(album);
            g_state.durMs     = durMs;
            g_state.rate      = rate;
        }

        if (changed) {

            std::vector<unsigned char> rgba;
            int w = 0, h = 0;
            const bool got = FetchArt(props.Thumbnail(), rgba, w, h);
            std::lock_guard<std::mutex> lk(g_mtx);
            g_state.artSerial++;
            if (got) {
                g_state.artRgba.swap(rgba);
                g_state.artW = w;
                g_state.artH = h;
                g_state.hasArt = true;
            } else {
                g_state.artRgba.clear();
                g_state.artW = g_state.artH = 0;
                g_state.hasArt = false;
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_state.available = false;
        g_state.playing   = false;
    }
}

void PollLoop() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    wmc::GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
    try {
        mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    } catch (...) {
    }

    while (g_running.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lk(g_pollMtx);
            g_pollCv.wait_for(lk, milliseconds(400), [] {
                return g_wake || !g_running.load(std::memory_order_relaxed);
            });
            g_wake = false;
        }
        if (!g_running.load(std::memory_order_relaxed))
            break;
        if (!mgr) {
            try {
                mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            } catch (...) {
            }
        }
        if (mgr) {
            try {
                PollOnce(mgr);
            } catch (...) {
            }
        }
    }
    try {
        winrt::uninit_apartment();
    } catch (...) {
    }
}

std::string UrlEscape(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

bool HttpGet(const std::string& pathAndQuery, std::string& body) {
    body.clear();
    HINTERNET ses = WinHttpOpen(L"APEX ALL IN ONE/1.0 (local media HUD)",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr)
        return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);

    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (con != nullptr) {
        std::wstring wpath(pathAndQuery.begin(), pathAndQuery.end());
        HINTERNET req = WinHttpOpenRequest(con, L"GET", wpath.c_str(), nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
        if (req != nullptr) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE |
                                              WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    ok = true;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
                            break;
                        std::vector<char> chunk(avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0)
                            break;
                        body.append(chunk.data(), read);
                        if (body.size() > 512u * 1024u)
                            break;
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

std::string JsonStringAfter(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos)
        return {};
    p += pat.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                               json[p] == '\r' || json[p] == '\n'))
        ++p;
    if (p >= json.size() || json[p] != ':')
        return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                               json[p] == '\r' || json[p] == '\n'))
        ++p;
    if (p + 4 <= json.size() && json.compare(p, 4, "null") == 0)
        return {};
    if (p >= json.size() || json[p] != '"')
        return {};
    ++p;
    std::string out;
    while (p < json.size() && json[p] != '"') {
        char c = json[p];
        if (c == '\\' && p + 1 < json.size()) {
            const char e = json[++p];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case '"': out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/');  break;
            case 'u': {
                unsigned cp = 0;
                bool good = p + 4 < json.size();
                for (int i = 1; i <= 4 && good; ++i) {
                    const char h = json[p + i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')      cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else good = false;
                }
                if (!good)
                    break;
                p += 4;

                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 < json.size() &&
                    json[p + 1] == '\\' && json[p + 2] == 'u') {
                    unsigned lo = 0;
                    bool loGood = true;
                    for (int i = 1; i <= 4; ++i) {
                        const char h = json[p + 2 + i];
                        lo <<= 4;
                        if (h >= '0' && h <= '9')      lo |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned>(h - 'A' + 10);
                        else loGood = false;
                    }
                    if (loGood && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default:
                out.push_back(e);
                break;
            }
        } else {
            out.push_back(c);
        }
        ++p;
    }
    return out;
}

long long ParseTimestamp(const std::string& line, size_t& bodyStart) {
    bodyStart = std::string::npos;
    if (line.size() < 4 || line[0] != '[')
        return -1;
    size_t p = 1;
    auto readNum = [&](long long& v, int* digits) -> bool {
        if (p >= line.size() || line[p] < '0' || line[p] > '9')
            return false;
        v = 0;
        int d = 0;
        while (p < line.size() && line[p] >= '0' && line[p] <= '9') {
            v = v * 10 + (line[p] - '0');
            ++p;
            ++d;
        }
        if (digits != nullptr)
            *digits = d;
        return true;
    };

    long long min = 0, sec = 0, frac = 0;
    int fracDigits = 0;
    if (!readNum(min, nullptr))
        return -1;
    if (p >= line.size() || line[p] != ':')
        return -1;
    ++p;
    if (!readNum(sec, nullptr))
        return -1;
    if (p < line.size() && (line[p] == '.' || line[p] == ':')) {
        ++p;
        if (!readNum(frac, &fracDigits))
            return -1;
    }
    if (p >= line.size() || line[p] != ']')
        return -1;
    ++p;

    long long ms = 0;
    if (fracDigits == 1)      ms = frac * 100;
    else if (fracDigits == 2) ms = frac * 10;
    else if (fracDigits == 3) ms = frac;
    else if (fracDigits > 3) {
        while (fracDigits > 3) {
            frac /= 10;
            --fracDigits;
        }
        ms = frac;
    }
    bodyStart = p;
    return min * 60000 + sec * 1000 + ms;
}

void ParseLyricsBody(const std::string& text, std::vector<LyricLine>& out) {
    out.clear();
    size_t pos = 0;
    bool anySynced = false;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        pos = end + 1;
        if (line.empty()) {
            if (pos > text.size())
                break;
            continue;
        }
        size_t body = std::string::npos;
        const long long t = ParseTimestamp(line, body);
        LyricLine ll;
        ll.tMs = t;
        if (t >= 0 && body != std::string::npos) {
            ll.text = line.substr(body);
            anySynced = true;
        } else {
            ll.tMs = -1;
            ll.text = line;
        }
        out.push_back(std::move(ll));
        if (pos > text.size())
            break;
    }

    if (anySynced) {
        std::vector<LyricLine> onlySynced;
        for (const LyricLine& l : out)
            if (l.tMs >= 0)
                onlySynced.push_back(l);
        if (!onlySynced.empty())
            out.swap(onlySynced);
    } else {
        for (LyricLine& l : out)
            l.tMs = -1;
    }
}

void LyricsWorker() {
    while (g_running.load(std::memory_order_relaxed)) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_jobMtx);
            g_jobCv.wait_for(lk, milliseconds(200), [] {
                return g_jobPending || !g_running.load(std::memory_order_relaxed);
            });
            if (!g_running.load(std::memory_order_relaxed))
                break;
            if (!g_jobPending)
                continue;
            job = g_job;
            g_jobPending = false;
        }

        std::string status = "none";
        std::vector<LyricLine> lines;

        std::string path = "/api/get?artist_name=" + UrlEscape(job.artist) +
                           "&track_name=" + UrlEscape(job.track);
        std::string body;
        if (HttpGet(path, body)) {
            std::string text = JsonStringAfter(body, "syncedLyrics");
            if (text.empty())
                text = JsonStringAfter(body, "plainLyrics");
            if (!text.empty()) {
                ParseLyricsBody(text, lines);
                if (!lines.empty())
                    status = "ok";
            }
        }

        std::lock_guard<std::mutex> lk(g_lyrMtx);
        if (job.id == g_reqId) {
            g_lyr.key    = job.key;
            g_lyr.status = status;
            g_lyr.lines  = std::move(lines);
        }
    }
}

}

void Start() {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true))
        return;
    g_pollThread  = std::thread(PollLoop);
    g_lyrThread   = std::thread(LyricsWorker);
}

void Stop() {
    if (!g_running.exchange(false))
        return;
    g_pollCv.notify_all();
    g_jobCv.notify_all();
    if (g_pollThread.joinable())
        g_pollThread.join();
    if (g_lyrThread.joinable())
        g_lyrThread.join();
}

bool Snapshot(NowPlaying& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    out.available = g_state.available;
    out.playing   = g_state.playing;
    out.title     = g_state.title;
    out.artist    = g_state.artist;
    out.album     = g_state.album;
    out.durMs     = g_state.durMs;
    out.artSerial = g_state.artSerial;
    out.hasArt    = g_state.hasArt;

    long long pos = g_state.posBase100 / 10000LL;
    if (g_state.available && g_state.playing) {
        const long long el =
            duration_cast<milliseconds>(steady_clock::now() - g_state.posWall).count();
        pos += static_cast<long long>(static_cast<double>(el) * g_state.rate);
    }
    if (g_state.durMs > 0 && pos > g_state.durMs)
        pos = g_state.durMs;
    if (pos < 0)
        pos = 0;
    out.posMs = pos;
    return out.available;
}

bool CopyArt(unsigned serial, std::vector<unsigned char>& rgba, int& w, int& h) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_state.hasArt || g_state.artSerial == serial)
        return false;
    rgba = g_state.artRgba;
    w = g_state.artW;
    h = g_state.artH;
    return true;
}

void Post(Cmd c) {
    {
        std::lock_guard<std::mutex> lk(g_cmdMtx);
        g_cmds.push_back(c);
    }
    {
        std::lock_guard<std::mutex> lk(g_pollMtx);
        g_wake = true;
    }
    g_pollCv.notify_one();
}

void Seek(long long ms) {
    if (ms < 0)
        ms = 0;
    {
        std::lock_guard<std::mutex> lk(g_cmdMtx);
        g_seeks.push_back(ms);
    }
    {
        std::lock_guard<std::mutex> lk(g_pollMtx);
        g_wake = true;
    }
    g_pollCv.notify_one();
}

void RequestLyrics(const std::string& artist, const std::string& track) {
    if (track.empty())
        return;
    const std::string key = TrackKey(artist, track);
    unsigned long long id = 0;
    {
        std::lock_guard<std::mutex> lk(g_lyrMtx);
        if (g_reqKey == key)
            return;
        g_reqKey = key;
        id = ++g_reqId;
        g_lyr.key    = key;
        g_lyr.status = "loading";
        g_lyr.lines.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_jobMtx);
        g_job        = Job{id, artist, track, key};
        g_jobPending = true;
    }
    g_jobCv.notify_one();
}

bool LyricsSnapshot(std::vector<LyricLine>& out, std::string& status,
                    std::string& key) {
    std::lock_guard<std::mutex> lk(g_lyrMtx);
    out    = g_lyr.lines;
    status = g_lyr.status;
    key    = g_lyr.key;
    return status == "ok";
}

}
