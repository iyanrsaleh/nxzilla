// nxzilla - jembatan terminal ke server remote.
//
// User membuka terminal di folder proyek, jalankan `nxzilla`, memasukkan
// konfigurasi ssh/ftp, lalu berkas proyek remote masuk ke folder kerja (cd).
// Perubahan dikirim balik ke server lewat perintah (push/pull).
//
// Dua lingkungan:
//   - proyek : dijalankan di dalam folder yang punya .nxzilla.json -> sinkron
//   - global : dijalankan di luar proyek -> daftar semua proyek Nxzilla
//
// Windows & Linux, satu basis kode. Backend koneksi memakai subprocess
// OpenSSH (ssh/sftp/scp) sehingga tanpa dependensi pihak ketiga.

#define NXZILLA_VERSION "1.2.0"

#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <chrono>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>

// ===========================================================================
// Umum
// ===========================================================================
static void Out(const std::string& s) {
    if (!s.empty()) fwrite(s.data(), 1, s.size(), stdout);
}

// Palet warna (mengikuti NxPort).
static const char* kOrange = "\x1b[1;38;2;233;84;32m";
static const char* kReset  = "\x1b[0m";
static const char* kGray   = "\x1b[90m";
static const char* kSel    = "\x1b[1;37;48;2;233;84;32m";
static const char* kStatus = "\x1b[1;30;47m";

// ===========================================================================
// Platform
// ===========================================================================
#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <wincred.h>
#include <conio.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

static const char kPathSep = '\\';

static void PlatformInit() {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
}

static std::string CurrentDir() {
    char buf[MAX_PATH * 4];
    if (_getcwd(buf, sizeof(buf))) return buf;
    return ".";
}

static std::string ConfigDir() {
    const char* appdata = getenv("APPDATA");
    if (appdata && *appdata) return std::string(appdata) + "\\Nxzilla";
    const char* profile = getenv("USERPROFILE");
    if (profile && *profile) return std::string(profile) + "\\Nxzilla";
    return ".";
}

static void MkDir(const std::string& p) { _mkdir(p.c_str()); }

static bool IsTty() { return _isatty(_fileno(stdin)) != 0; }

// ---- Input tombol ----
enum Key {
    KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_CHAR
};

static int  g_lastKeyChar = 0;
static bool g_rawInput = false;

static Key ReadKey(int timeoutMs) {
    g_lastKeyChar = 0;
    int waited = 0;
    while (!_kbhit()) {
        Sleep(20);
        waited += 20;
        if (waited >= timeoutMs) return KEY_NONE;
    }
    int ch = _getch();
    if (ch == 224 || ch == 0) {
        int arrow = _getch();
        if (arrow == 72) return KEY_UP;
        if (arrow == 80) return KEY_DOWN;
        if (arrow == 75) return KEY_LEFT;
        if (arrow == 77) return KEY_RIGHT;
        return KEY_NONE;
    }
    if (ch == 13 || ch == 10) return KEY_ENTER;
    if (ch == 27) return KEY_ESC;
    if (ch == 8) return KEY_BACKSPACE;
    if (g_rawInput) {
        if (ch >= 32 && ch < 127) { g_lastKeyChar = ch; return KEY_CHAR; }
        return KEY_NONE;
    }
    if (ch == 'q' || ch == 'Q') return KEY_ESC;  // 'q' = keluar dari layar skrg
    if (ch >= 32 && ch < 127) { g_lastKeyChar = ch; return KEY_CHAR; }
    return KEY_NONE;
}

struct RawTerminalScope {
    RawTerminalScope() {}
    ~RawTerminalScope() {}
};

static void GetTerminalSize(int& rows, int& cols) {
    rows = 30;
    cols = 100;
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
}

#else  // POSIX / Linux

#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <dirent.h>
#include <utime.h>

static const char kPathSep = '/';

static void PlatformInit() {}

static std::string CurrentDir() {
    char buf[4096];
    if (getcwd(buf, sizeof(buf))) return buf;
    return ".";
}

static std::string ConfigDir() {
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/nxzilla";
    const char* home = getenv("HOME");
    if (home && *home) return std::string(home) + "/.config/nxzilla";
    return ".";
}

static void MkDir(const std::string& p) { mkdir(p.c_str(), 0755); }

static bool IsTty() { return isatty(STDIN_FILENO) != 0; }

enum Key {
    KEY_NONE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_CHAR
};

static int  g_lastKeyChar = 0;
static bool g_rawInput = false;

struct RawTerminalScope {
    termios orig{};
    bool active = false;
    RawTerminalScope() {
        if (tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios raw = orig;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) active = true;
        }
    }
    ~RawTerminalScope() {
        if (active) tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    }
};

static Key ReadKey(int timeoutMs) {
    g_lastKeyChar = 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int s = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
    if (s <= 0) return KEY_NONE;

    char ch = 0;
    if (read(STDIN_FILENO, &ch, 1) <= 0) return KEY_NONE;
    if (ch == 27) {
        char seq[2] = {0};
        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
            if (seq[0] == '[') {
                if (seq[1] == 'A') return KEY_UP;
                if (seq[1] == 'B') return KEY_DOWN;
                if (seq[1] == 'C') return KEY_RIGHT;
                if (seq[1] == 'D') return KEY_LEFT;
            }
        }
        return KEY_ESC;
    }
    if (ch == 13 || ch == 10) return KEY_ENTER;
    if (ch == 127 || ch == 8) return KEY_BACKSPACE;
    if (g_rawInput) {
        if (ch >= 32 && ch < 127) { g_lastKeyChar = ch; return KEY_CHAR; }
        return KEY_NONE;
    }
    if (ch == 'q' || ch == 'Q') return KEY_ESC;
    if (ch >= 32 && ch < 127) { g_lastKeyChar = ch; return KEY_CHAR; }
    return KEY_NONE;
}

static void GetTerminalSize(int& rows, int& cols) {
    rows = 30;
    cols = 100;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
}

#endif  // _WIN32

// Tidur portabel (milidetik).
static void SleepMs(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
#endif
}

// ===========================================================================
// Berkas & path
// ===========================================================================
static bool FileExists(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Apakah path ada (berkas ATAU folder)?
static bool PathExists(const std::string& path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
#else
    struct stat stt{};
    return stat(path.c_str(), &stt) == 0;
#endif
}

static std::string ReadFileText(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

static bool WriteFileText(const std::string& path, const std::string& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t w = fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return w == data.size();
}

static void EnsureDir(const std::string& path) {
    if (path.empty()) return;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || path[i] == '\\') {
            bool driveRoot = (cur.size() == 3 && cur[1] == ':');
            if (cur.size() > 1 && !driveRoot) MkDir(cur);
        }
    }
    MkDir(path);
}

static bool JoinPath(const std::string& dir, const std::string& name, std::string& out) {
    if (dir.empty()) return false;
    out = dir;
    if (out.back() != '/' && out.back() != '\\') out += kPathSep;
    out += name;
    return true;
}

struct LocalEntry {
    std::string name;
    bool isDir = false;
};

static std::vector<LocalEntry> ListLocalDir(const std::string& dir) {
    std::vector<LocalEntry> out;
#ifdef _WIN32
    std::string pattern = dir;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/') pattern += '\\';
    pattern += "*";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string nm = fd.cFileName;
        if (nm == "." || nm == "..") continue;
        LocalEntry e;
        e.name = nm;
        e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(std::move(e));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string nm = de->d_name;
        if (nm == "." || nm == "..") continue;
        LocalEntry e;
        e.name = nm;
        e.isDir = (de->d_type == DT_DIR);
        if (de->d_type == DT_UNKNOWN) {
            std::string full = dir + "/" + nm;
            struct stat stt{};
            if (stat(full.c_str(), &stt) == 0) e.isDir = S_ISDIR(stt.st_mode);
        }
        out.push_back(std::move(e));
    }
    closedir(d);
#endif
    return out;
}

static long long FileSize(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa))
        return ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return -1;
#else
    struct stat stt{};
    if (stat(path.c_str(), &stt) == 0) return (long long)stt.st_size;
    return -1;
#endif
}

// Waktu modifikasi lokal (epoch detik). return 0 bila gagal.
static long long FileMTime(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) {
        ULARGE_INTEGER u;
        u.LowPart = fa.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fa.ftLastWriteTime.dwHighDateTime;
        const unsigned long long EPOCH = 116444736000000000ULL;
        if (u.QuadPart > EPOCH) return (long long)((u.QuadPart - EPOCH) / 10000000ULL);
    }
    return 0;
#else
    struct stat stt{};
    if (stat(path.c_str(), &stt) == 0) return (long long)stt.st_mtime;
    return 0;
#endif
}

// ---- SHA-1 (tanpa dependensi) ----
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    unsigned char buf[64];
    size_t bufLen = 0;
    unsigned long long total = 0;

    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

    void process(const unsigned char* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
                   (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const unsigned char* data, size_t n) {
        total += n;
        for (size_t i = 0; i < n; ++i) {
            buf[bufLen++] = data[i];
            if (bufLen == 64) { process(buf); bufLen = 0; }
        }
    }

    std::string hexdigest() {
        unsigned long long bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (bufLen != 56) update(&zero, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char)(bits >> (56 - i*8));
        update(lenb, 8);
        char out[41];
        for (int i = 0; i < 5; ++i) snprintf(out + i*8, 9, "%08x", h[i]);
        return std::string(out, 40);
    }
};

static std::string FileSha1(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    Sha1 s;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.update(buf, n);
    fclose(f);
    return s.hexdigest();
}

// Set waktu modifikasi lokal (epoch detik). Dipakai agar mtime lokal
// mengikuti remote setelah pull (seperti rsync -t).
static bool SetFileMTime(const std::string& path, long long epoch) {
#ifdef _WIN32
    HANDLE h = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const unsigned long long EPOCH = 116444736000000000ULL;
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)epoch * 10000000ULL + EPOCH;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    bool ok = SetFileTime(h, nullptr, nullptr, &ft) != 0;
    CloseHandle(h);
    return ok;
#else
    struct utimbuf ut;
    ut.actime = (time_t)epoch;
    ut.modtime = (time_t)epoch;
    return utime(path.c_str(), &ut) == 0;
#endif
}

static std::string EscapeJson(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '\\') r += "\\\\";
        else if (c == '"') r += "\\\"";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r;
}

// Telusuri .nxzilla.json dari `dir` ke atas.
static bool FindProjectRoot(const std::string& dir, std::string& root) {
    std::string cur = dir;
    for (;;) {
        std::string candidate;
        if (JoinPath(cur, ".nxzilla.json", candidate) && FileExists(candidate)) {
            root = cur;
            return true;
        }
        size_t slash = cur.find_last_of("/\\");
        if (slash == std::string::npos) break;
        if (slash == 0) break;
#ifdef _WIN32
        if (slash == 2 && cur.size() >= 2 && cur[1] == ':') {
            cur = cur.substr(0, 3);
            if (JoinPath(cur, ".nxzilla.json", candidate) && FileExists(candidate)) {
                root = cur;
                return true;
            }
            break;
        }
#endif
        cur = cur.substr(0, slash);
        if (cur.empty()) break;
    }
    return false;
}

// ===========================================================================
// JSON minimal (cukup untuk file konfigurasi Nxzilla)
// ===========================================================================
struct JVal {
    enum Type { NUL, BOO, NUM, STR, ARR, OBJ } type = NUL;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<JVal> items;
    std::vector<std::pair<std::string, JVal>> fields;

    const JVal* find(const std::string& key) const {
        if (type != OBJ) return nullptr;
        for (auto& kv : fields) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string str(const std::string& key, const std::string& def = "") const {
        const JVal* v = find(key);
        return (v && v->type == STR) ? v->text : def;
    }
    const JVal* arr(const std::string& key) const {
        const JVal* v = find(key);
        return (v && v->type == ARR) ? v : nullptr;
    }
};

struct JParser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit JParser(const std::string& in) : s(in) {}

    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }

    bool parseStr(std::string& out) {
        if (i >= s.size() || s[i] != '"') { ok = false; return false; }
        i++;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else if (e == 'r') out += '\r';
                else if (e == '"') out += '"';
                else if (e == '\\') out += '\\';
                else if (e == '/') out += '/';
                else if (e == 'b') out += '\b';
                else if (e == 'f') out += '\f';
                else if (e == 'u') {
                    unsigned code = 0;
                    for (int k = 0; k < 4 && i < s.size(); ++k) {
                        char h = s[i++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                    }
                    if (code < 0x80) out += (char)code;
                } else out += e;
            } else {
                out += c;
            }
        }
        ok = false;
        return false;
    }

    bool parseNum(JVal& out) {
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.' ||
                                s[i] == 'e' || s[i] == 'E' || s[i] == '-' || s[i] == '+'))
            i++;
        if (i == start) { ok = false; return false; }
        out.type = JVal::NUM;
        out.number = atof(s.substr(start, i - start).c_str());
        return true;
    }

    bool parseArr(JVal& out) {
        out.type = JVal::ARR;
        i++;
        ws();
        if (i < s.size() && s[i] == ']') { i++; return true; }
        while (i < s.size()) {
            JVal v;
            if (!parse(v)) return false;
            out.items.push_back(std::move(v));
            ws();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; return true; }
            ok = false;
            return false;
        }
        ok = false;
        return false;
    }

    bool parseObj(JVal& out) {
        out.type = JVal::OBJ;
        i++;
        ws();
        if (i < s.size() && s[i] == '}') { i++; return true; }
        while (i < s.size()) {
            ws();
            if (i >= s.size() || s[i] != '"') { ok = false; return false; }
            std::string key;
            if (!parseStr(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') { ok = false; return false; }
            i++;
            JVal v;
            if (!parse(v)) return false;
            out.fields.emplace_back(std::move(key), std::move(v));
            ws();
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; return true; }
            ok = false;
            return false;
        }
        ok = false;
        return false;
    }

    bool parse(JVal& out) {
        ws();
        if (i >= s.size()) { ok = false; return false; }
        char c = s[i];
        if (c == '{') return parseObj(out);
        if (c == '[') return parseArr(out);
        if (c == '"') { out.type = JVal::STR; return parseStr(out.text); }
        if (s.compare(i, 4, "true") == 0) { i += 4; out.type = JVal::BOO; out.boolean = true; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out.type = JVal::BOO; out.boolean = false; return true; }
        if (s.compare(i, 4, "null") == 0) { i += 4; out.type = JVal::NUL; return true; }
        return parseNum(out);
    }
};

// ===========================================================================
// Model data
// ===========================================================================
struct Profile {
    std::string name, protocol, host, user, password, key, remotePath;
    int port = 0;
};

struct Project {
    std::string name, localPath, profile, remotePath, lastSync, lastStatus;
};

static std::string ProfilesPath() {
    std::string p;
    JoinPath(ConfigDir(), "connections.json", p);
    return p;
}
static std::string ProjectsPath() {
    std::string p;
    JoinPath(ConfigDir(), "projects.json", p);
    return p;
}

// ===========================================================================
// Obfuskasi kredensial (bukan enkripsi kuat; sekadar menghindari teks polos)
// ===========================================================================
static const char* kObfKey = "nxzilla-v1-obfuscate";
static const char* kObfMagic = "enc1:";

static std::string Obfuscate(const std::string& plain) {
    if (plain.empty()) return "";
    std::string out = kObfMagic;
    size_t klen = strlen(kObfKey);
    for (size_t i = 0; i < plain.size(); ++i) {
        unsigned char c = (unsigned char)plain[i] ^ (unsigned char)kObfKey[i % klen];
        c = (unsigned char)(c + 0x21);  // hindari 0x00 agar aman di JSON/hex
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", c);
        out += hex;
    }
    return out;
}

static std::string Deobfuscate(const std::string& stored) {
    if (stored.rfind(kObfMagic, 0) != 0) return stored;  // polos (legacy)
    std::string hex = stored.substr(strlen(kObfMagic));
    std::string out;
    size_t klen = strlen(kObfKey);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hex[i], lo = hex[i + 1];
        auto val = [](int h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
            return 0;
        };
        unsigned char c = (unsigned char)((val(hi) << 4) | val(lo));
        c = (unsigned char)(c - 0x21);
        c ^= (unsigned char)kObfKey[(i / 2) % klen];
        out += (char)c;
    }
    return out;
}

// ===========================================================================
// OS keyring (Windows Credential Manager; Linux via secret-tool/libsecret).
// ===========================================================================
static int RunCapture(const std::string& cmd, std::string& out);
static int RunWithInput(const std::string& cmd, const std::string& input, std::string& out);
static std::string ShellQuoteD(const std::string& s);

[[maybe_unused]] static std::string KeyringTarget(const std::string& profileName) {
    return "Nxzilla/" + profileName;
}

// Linux: apakah `secret-tool` tersedia?
[[maybe_unused]] static bool HaveSecretTool() {
#ifdef _WIN32
    return false;
#else
    static int cached = -1;
    if (cached < 0) {
        std::string out;
        cached = (RunCapture("command -v secret-tool >/dev/null 2>&1 && echo yes", out) == 0 &&
                  out.find("yes") != std::string::npos) ? 1 : 0;
    }
    return cached == 1;
#endif
}

// Simpan password ke keyring OS. return true bila berhasil.
static bool KeyringStore(const std::string& profileName, const std::string& password) {
#ifdef _WIN32
    std::string target = KeyringTarget(profileName);
    CREDENTIALA cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<char*>(target.c_str());
    cred.CredentialBlobSize = (DWORD)password.size();
    cred.CredentialBlob = (LPBYTE)const_cast<char*>(password.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = const_cast<char*>(profileName.c_str());
    return CredWriteA(&cred, 0) != 0;
#else
    if (!HaveSecretTool()) return false;
    // secret-tool store membaca secret dari stdin.
    std::string cmd = "secret-tool store --label=" + ShellQuoteD(KeyringTarget(profileName)) +
                      " nxzilla " + ShellQuoteD(profileName);
    std::string out;
    return RunWithInput(cmd, password + "\n", out) == 0;
#endif
}

// Hapus password dari keyring.
static void KeyringDelete(const std::string& profileName) {
#ifdef _WIN32
    std::string target = KeyringTarget(profileName);
    CredDeleteA(target.c_str(), CRED_TYPE_GENERIC, 0);
#else
    if (!HaveSecretTool()) return;
    std::string out;
    RunCapture("secret-tool clear nxzilla " + ShellQuoteD(profileName) + " 2>&1", out);
#endif
}

// Ambil password dari keyring. return "" bila tidak ada.
static std::string KeyringGet(const std::string& profileName) {
#ifdef _WIN32
    std::string target = KeyringTarget(profileName);
    PCREDENTIALA cred = nullptr;
    if (!CredReadA(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) return "";
    std::string pw;
    if (cred->CredentialBlob && cred->CredentialBlobSize)
        pw.assign((char*)cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return pw;
#else
    if (!HaveSecretTool()) return "";
    std::string out;
    if (RunCapture("secret-tool lookup nxzilla " + ShellQuoteD(profileName) + " 2>/dev/null", out) != 0)
        return "";
    // buang newline akhir
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
#endif
}

// Password efektif: env NXZILLA_PASSWORD menang untuk skrip/non-interaktif.
// Urutan: env -> keyring OS -> nilai di file (obfuskasi/legacy).
static std::string EffectivePassword(const Profile& p) {
    const char* env = getenv("NXZILLA_PASSWORD");
    if (env && *env) return env;
    std::string kr = KeyringGet(p.name);
    if (!kr.empty()) return kr;
    return p.password;
}

static std::vector<Profile> LoadProfiles() {
    std::vector<Profile> out;
    std::string raw = ReadFileText(ProfilesPath());
    if (raw.empty()) return out;
    JVal root;
    JParser jp(raw);
    if (!jp.parse(root)) return out;
    const JVal* arr = root.arr("profiles");
    if (!arr) return out;
    for (const JVal& e : arr->items) {
        Profile p;
        p.name = e.str("name");
        p.protocol = e.str("protocol", "ssh");
        p.host = e.str("host");
        p.user = e.str("user");
        p.password = Deobfuscate(e.str("password"));
        p.key = e.str("key");
        p.remotePath = e.str("remotePath");
        const JVal* port = e.find("port");
        p.port = (port && port->type == JVal::NUM) ? (int)port->number : 0;
        out.push_back(std::move(p));
    }
    return out;
}

static std::vector<Project> LoadProjects() {
    std::vector<Project> out;
    std::string raw = ReadFileText(ProjectsPath());
    if (raw.empty()) return out;
    JVal root;
    JParser jp(raw);
    if (!jp.parse(root)) return out;
    const JVal* arr = root.arr("projects");
    if (!arr) return out;
    for (const JVal& e : arr->items) {
        Project p;
        p.name = e.str("name");
        p.localPath = e.str("localPath");
        p.profile = e.str("profile");
        p.remotePath = e.str("remotePath");
        p.lastSync = e.str("lastSync");
        p.lastStatus = e.str("lastStatus", "unknown");
        out.push_back(std::move(p));
    }
    return out;
}

static std::vector<std::string> LoadProjectIgnore(const std::string& projectDir) {
    std::vector<std::string> out;
    std::string path;
    if (!JoinPath(projectDir, ".nxzilla.json", path)) return out;
    std::string raw = ReadFileText(path);
    if (raw.empty()) return out;
    JVal root;
    JParser jp(raw);
    if (!jp.parse(root)) return out;
    const JVal* arr = root.arr("ignore");
    if (!arr) return out;
    for (const JVal& e : arr->items)
        if (e.type == JVal::STR) out.push_back(e.text);
    return out;
}

// Satu target lingkungan (nama -> profil + remotePath).
struct EnvEntry {
    std::string name, profile, remotePath;
};

// Baca seluruh isi .nxzilla.json: profil/remotePath efektif (dari 'active' bila
// ada), daftar environments, dan nama active.
static bool LoadProjectFull(const std::string& projectDir, std::string& profile,
                            std::string& remotePath, std::vector<EnvEntry>& envs,
                            std::string& active) {
    std::string path;
    if (!JoinPath(projectDir, ".nxzilla.json", path)) return false;
    std::string raw = ReadFileText(path);
    if (raw.empty()) return false;
    JVal root;
    JParser jp(raw);
    if (!jp.parse(root)) return false;
    profile = root.str("profile");
    remotePath = root.str("remotePath");
    active = root.str("active");
    const JVal* arr = root.arr("environments");
    if (arr) {
        for (const JVal& e : arr->items) {
            EnvEntry en;
            en.name = e.str("name");
            en.profile = e.str("profile");
            en.remotePath = e.str("remotePath");
            if (!en.name.empty()) envs.push_back(std::move(en));
        }
    }
    // Bila ada 'active', timpa profil/remotePath efektif.
    if (!active.empty()) {
        for (const auto& en : envs)
            if (en.name == active) { profile = en.profile; remotePath = en.remotePath; break; }
    }
    return true;
}

// Baca profil & remotePath efektif dari .nxzilla.json (menghormati 'active').
static bool LoadProjectConfig(const std::string& projectDir, std::string& profile,
                              std::string& remotePath) {
    std::vector<EnvEntry> envs;
    std::string active;
    return LoadProjectFull(projectDir, profile, remotePath, envs, active);
}

// Tulis .nxzilla.json (mempertahankan environments bila ada).
static bool SaveProjectConfig(const std::string& projectDir, const std::string& profile,
                              const std::string& remotePath,
                              const std::vector<std::string>& ignore) {
    // Baca environments lama agar tidak hilang.
    std::vector<EnvEntry> envs;
    std::string p0, r0, active0;
    LoadProjectFull(projectDir, p0, r0, envs, active0);

    std::string j = "{\n";
    j += "  \"profile\": \"" + EscapeJson(profile) + "\",\n";
    j += "  \"remotePath\": \"" + EscapeJson(remotePath) + "\",\n";
    if (!envs.empty()) {
        j += "  \"active\": \"" + EscapeJson(active0) + "\",\n";
        j += "  \"environments\": [\n";
        for (size_t i = 0; i < envs.size(); ++i) {
            j += "    { \"name\": \"" + EscapeJson(envs[i].name) + "\", \"profile\": \"" +
                 EscapeJson(envs[i].profile) + "\", \"remotePath\": \"" +
                 EscapeJson(envs[i].remotePath) + "\" }";
            if (i + 1 < envs.size()) j += ",";
            j += "\n";
        }
        j += "  ],\n";
    }
    j += "  \"ignore\": [";
    for (size_t i = 0; i < ignore.size(); ++i) {
        j += "\"" + EscapeJson(ignore[i]) + "\"";
        if (i + 1 < ignore.size()) j += ", ";
    }
    j += "]\n}\n";
    std::string path;
    if (!JoinPath(projectDir, ".nxzilla.json", path)) return false;
    return WriteFileText(path, j);
}

// Tulis .nxzilla.json dengan daftar environments + active tertentu.
static bool SaveEnvironments(const std::string& projectDir, const std::vector<EnvEntry>& envs,
                             const std::string& active, const std::vector<std::string>& ignore) {
    std::string profile, remotePath;
    // Nilai efektif = profil/remote dari active (bila ada).
    for (const auto& e : envs)
        if (e.name == active) { profile = e.profile; remotePath = e.remotePath; break; }
    std::string j = "{\n";
    j += "  \"profile\": \"" + EscapeJson(profile) + "\",\n";
    j += "  \"remotePath\": \"" + EscapeJson(remotePath) + "\",\n";
    j += "  \"active\": \"" + EscapeJson(active) + "\",\n";
    j += "  \"environments\": [\n";
    for (size_t i = 0; i < envs.size(); ++i) {
        j += "    { \"name\": \"" + EscapeJson(envs[i].name) + "\", \"profile\": \"" +
             EscapeJson(envs[i].profile) + "\", \"remotePath\": \"" +
             EscapeJson(envs[i].remotePath) + "\" }";
        if (i + 1 < envs.size()) j += ",";
        j += "\n";
    }
    j += "  ],\n";
    j += "  \"ignore\": [";
    for (size_t i = 0; i < ignore.size(); ++i) {
        j += "\"" + EscapeJson(ignore[i]) + "\"";
        if (i + 1 < ignore.size()) j += ", ";
    }
    j += "]\n}\n";
    std::string path;
    if (!JoinPath(projectDir, ".nxzilla.json", path)) return false;
    return WriteFileText(path, j);
}

static bool SaveProjects(const std::vector<Project>& projects) {
    std::string j = "{\n  \"projects\": [\n";
    for (size_t i = 0; i < projects.size(); ++i) {
        const Project& p = projects[i];
        j += "    {\n";
        j += "      \"name\": \"" + EscapeJson(p.name) + "\",\n";
        j += "      \"localPath\": \"" + EscapeJson(p.localPath) + "\",\n";
        j += "      \"profile\": \"" + EscapeJson(p.profile) + "\",\n";
        j += "      \"remotePath\": \"" + EscapeJson(p.remotePath) + "\",\n";
        j += "      \"lastSync\": \"" + EscapeJson(p.lastSync) + "\",\n";
        j += "      \"lastStatus\": \"" + EscapeJson(p.lastStatus.empty() ? "unknown" : p.lastStatus) + "\"\n";
        j += "    }";
        if (i + 1 < projects.size()) j += ",";
        j += "\n";
    }
    j += "  ]\n}\n";
    EnsureDir(ConfigDir());
    return WriteFileText(ProjectsPath(), j);
}

static bool SaveProfiles(const std::vector<Profile>& profiles) {
    std::string j = "{\n  \"profiles\": [\n";
    for (size_t i = 0; i < profiles.size(); ++i) {
        const Profile& p = profiles[i];
        j += "    {\n";
        j += "      \"name\": \"" + EscapeJson(p.name) + "\",\n";
        j += "      \"protocol\": \"" + EscapeJson(p.protocol.empty() ? "ftp" : p.protocol) + "\",\n";
        j += "      \"host\": \"" + EscapeJson(p.host) + "\",\n";
        j += "      \"port\": " + std::to_string(p.port) + ",\n";
        j += "      \"user\": \"" + EscapeJson(p.user) + "\",\n";
        // Password disimpan di keyring OS; di file hanya placeholder/obfuskasi
        // (fallback bila keyring tidak tersedia, mis. Linux tanpa libsecret).
        std::string stored;
        if (!p.password.empty()) {
            if (KeyringStore(p.name, p.password)) stored = "";       // aman di keyring
            else stored = Obfuscate(p.password);                     // fallback
        }
        j += "      \"password\": \"" + EscapeJson(stored) + "\",\n";
        j += "      \"key\": \"" + EscapeJson(p.key) + "\",\n";
        j += "      \"remotePath\": \"" + EscapeJson(p.remotePath) + "\"\n";
        j += "    }";
        if (i + 1 < profiles.size()) j += ",";
        j += "\n";
    }
    j += "  ]\n}\n";
    EnsureDir(ConfigDir());
    return WriteFileText(ProfilesPath(), j);
}

// ===========================================================================
// Subprocess
// ===========================================================================
// Pemindaian folder untuk menemukan proyek (folder ber-.nxzilla.json).
static void ScanForProjects(const std::string& dir, int depth,
                            std::vector<Project>& out) {
    if (depth > 6) return;
    std::string cfg;
    if (JoinPath(dir, ".nxzilla.json", cfg) && FileExists(cfg)) {
        Project p;
        std::string prof, rp;
        LoadProjectConfig(dir, prof, rp);
        // Nama proyek: nama folder.
        size_t s = dir.find_last_of("/\\");
        p.name = (s == std::string::npos) ? dir : dir.substr(s + 1);
        p.localPath = dir;
        p.profile = prof;
        p.remotePath = rp;
        p.lastStatus = "unknown";
        out.push_back(std::move(p));
        return;  // jangan telusuri lebih dalam dari sebuah proyek
    }
    for (const auto& e : ListLocalDir(dir)) {
        if (!e.isDir) continue;
        if (e.name == ".git" || e.name == "node_modules" || e.name == ".svn") continue;
        std::string child;
        JoinPath(dir, e.name, child);
        ScanForProjects(child, depth + 1, out);
    }
}

// Jalankan perintah, tangkap stdout, kembalikan exit code.
static int RunCapture(const std::string& cmd, std::string& out) {
    out.clear();
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "rb");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return -1;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
#ifdef _WIN32
    int rc = _pclose(p);
#else
    int rc = pclose(p);
#endif
    return rc;
}

// ===========================================================================
// Backend FTP / FTPS (via curl)
// ===========================================================================
// Amankan nilai agar tidak pecah di shell: bungkus kutip ganda.
static std::string ShellQuoteD(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    r += "\"";
    return r;
}

// Jalankan perintah sambil mengirim `input` ke stdin-nya (via file sementara).
[[maybe_unused]] static int RunWithInput(const std::string& cmd, const std::string& input, std::string& out) {
    EnsureDir(ConfigDir());
    std::string tmp;
    JoinPath(ConfigDir(), "stdin_tmp", tmp);
    WriteFileText(tmp, input);
    std::string full = cmd + " < " + ShellQuoteD(tmp);
    return RunCapture(full, out);
}

// Alamat dasar FTP: ftp://host:port/path
static std::string FtpBaseUrl(const Profile& pr, const std::string& remotePath) {
    std::string rp = remotePath.empty() ? "/" : remotePath;
    std::string url = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) url += ":" + std::to_string(pr.port);
    if (rp.empty() || rp[0] != '/') url += "/";
    url += rp;
    if (url.back() != '/') url += "/";
    return url;
}

// Jalankan curl dengan kredensial profil. flag = tambahan argumen curl.
static int CurlRun(const Profile& pr, const std::string& extra, std::string& out) {
    std::string cmd = "curl -sS";
    if (pr.protocol == "ftps") cmd += " --ssl-reqd";
    if (!pr.user.empty()) {
        cmd += " --user " + ShellQuoteD(pr.user + ":" + EffectivePassword(pr));
    }
    cmd += " --connect-timeout 15 --max-time 600 ";
    cmd += extra;
    cmd += " 2>&1";
    return RunCapture(cmd, out);
}

// Jalankan curl dengan file konfigurasi (-K) -> banyak transfer dalam SATU
// koneksi (jauh lebih cepat daripada satu proses curl per berkas).
static int CurlRunConfig(const Profile& pr, const std::string& cfgText, std::string& out) {
    EnsureDir(ConfigDir());
    std::string cfg;
    JoinPath(ConfigDir(), "curl_batch.cfg", cfg);
    WriteFileText(cfg, cfgText);
    std::string cmd = "curl -sS";
    if (pr.protocol == "ftps") cmd += " --ssl-reqd";
    if (!pr.user.empty())
        cmd += " --user " + ShellQuoteD(pr.user + ":" + EffectivePassword(pr));
    cmd += " --connect-timeout 15 --max-time 900";
    cmd += " -K " + ShellQuoteD(cfg) + " 2>&1";
    return RunCapture(cmd, out);
}

// Escape nilai untuk file config curl (kutip ganda).
static std::string CfgQuote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    r += "\"";
    return r;
}

// Jalankan perintah dengan terminal diwarisi (untuk editor). Blok sampai selesai.
static int RunInteractive(const std::string& cmd) {
    return system(cmd.c_str());
}

// Batch unduh: items = pasangan (remoteRel, localFile). Satu sesi curl.
static bool FtpBatchGet(const Profile& pr, const std::vector<std::pair<std::string, std::string>>& items,
                        std::string& err) {
    if (items.empty()) return true;
    std::string urlBase = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) urlBase += ":" + std::to_string(pr.port);
    std::string rp = pr.remotePath;
    if (rp.empty() || rp[0] != '/') rp = "/" + rp;
    if (rp.back() != '/') rp += "/";
    std::string cfg;
    for (const auto& it : items) {
        cfg += "url = " + CfgQuote(urlBase + rp + it.first) + "\n";
        cfg += "output = " + CfgQuote(it.second) + "\n";
    }
    std::string out;
    int rc = CurlRunConfig(pr, cfg, out);
    if (rc != 0) { err = out; return false; }
    return true;
}

// Batch unggah: items = pasangan (localFile, remoteRel). Satu sesi curl.
static bool FtpBatchPut(const Profile& pr, const std::vector<std::pair<std::string, std::string>>& items,
                        std::string& err) {
    if (items.empty()) return true;
    std::string urlBase = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) urlBase += ":" + std::to_string(pr.port);
    std::string rp = pr.remotePath;
    if (rp.empty() || rp[0] != '/') rp = "/" + rp;
    if (rp.back() != '/') rp += "/";
    std::string cfg;
    for (const auto& it : items) {
        cfg += "upload-file = " + CfgQuote(it.first) + "\n";
        cfg += "url = " + CfgQuote(urlBase + rp + it.second) + "\n";
    }
    std::string out;
    int rc = CurlRunConfig(pr, cfg, out);
    if (rc != 0) { err = out; return false; }
    return true;
}

// Uji koneksi: daftar isi folder remote.
static bool FtpList(const Profile& pr, const std::string& remotePath,
                    std::vector<std::string>& names, std::string& err) {
    std::string extra = " --list-only " + ShellQuoteD(FtpBaseUrl(pr, remotePath));
    std::string out;
    int rc = CurlRun(pr, extra, out);
    if (rc != 0) {
        err = out.empty() ? ("curl exit " + std::to_string(rc)) : out;
        return false;
    }
    names.clear();
    size_t i = 0;
    while (i < out.size()) {
        size_t e = out.find('\n', i);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        i = e + 1;
        if (line.empty() || line == "." || line == "..") continue;
        names.push_back(line);
    }
    return true;
}

// Tarik satu berkas remote ke path lokal.
static bool FtpGetFile(const Profile& pr, const std::string& remoteRel,
                       const std::string& localFile, std::string& err) {
    std::string url = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) url += ":" + std::to_string(pr.port);
    std::string rp = pr.remotePath;
    if (rp.empty() || rp[0] != '/') rp = "/" + rp;
    if (rp.back() != '/') rp += "/";
    url += rp + remoteRel;
    std::string extra = " -o " + ShellQuoteD(localFile) + " " + ShellQuoteD(url);
    std::string out;
    int rc = CurlRun(pr, extra, out);
    if (rc != 0) { err = out.empty() ? ("curl exit " + std::to_string(rc)) : out; return false; }
    return true;
}

// Rekursif: tarik seluruh isi folder remote ke direktori lokal.
static int FtpPullRecursive(const Profile& pr, const std::string& remotePath,
                            const std::string& localDir, int depth, std::string& err) {
    if (depth > 24) return 0;
    std::vector<std::string> names;
    if (!FtpList(pr, remotePath, names, err)) return -1;

    EnsureDir(localDir);
    int count = 0;
    for (const auto& name : names) {
        std::string childRemote = remotePath;
        if (childRemote.empty() || childRemote.back() != '/') childRemote += "/";
        childRemote += name;

        // Coba list sebagai folder; jika bisa, itu direktori.
        std::vector<std::string> sub;
        std::string suberr;
        bool isDir = FtpList(pr, childRemote, sub, suberr);

        std::string localChild;
        JoinPath(localDir, name, localChild);

        if (isDir) {
            int r = FtpPullRecursive(pr, childRemote, localChild, depth + 1, err);
            if (r < 0) return -1;
            count += r;
        } else {
            std::string gerr;
            std::string rel = childRemote;
            if (!pr.remotePath.empty() && rel.rfind(pr.remotePath, 0) == 0)
                rel = rel.substr(pr.remotePath.size());
            if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
            if (!FtpGetFile(pr, rel, localChild, gerr)) { err = gerr; continue; }
            count++;
        }
    }
    return count;
}

// Unggah satu berkas lokal ke path remote relatif.
static bool FtpPutFile(const Profile& pr, const std::string& remoteRel,
                       const std::string& localFile, std::string& err) {
    std::string url = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) url += ":" + std::to_string(pr.port);
    std::string rp = pr.remotePath;
    if (rp.empty() || rp[0] != '/') rp = "/" + rp;
    if (rp.back() != '/') rp += "/";
    url += rp + remoteRel;
    std::string extra = " -T " + ShellQuoteD(localFile) + " " + ShellQuoteD(url);
    std::string out;
    int rc = CurlRun(pr, extra, out);
    if (rc != 0) { err = out.empty() ? ("curl exit " + std::to_string(rc)) : out; return false; }
    return true;
}

// Buat folder remote (curl -Q "MKD path"). Gagal diabaikan (mungkin sudah ada).
static bool FtpMkdir(const Profile& pr, const std::string& remoteRel, std::string& err) {
    std::string rp = pr.remotePath;
    if (rp.empty() || rp[0] != '/') rp = "/" + rp;
    if (rp.back() != '/') rp += "/";
    std::string url = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) url += ":" + std::to_string(pr.port);
    url += "/";
    std::string devnull =
#ifdef _WIN32
        "NUL";
#else
        "/dev/null";
#endif
    // Buat tiap segmen bertingkat: a, a/b, a/b/c
    std::string acc = rp;
    if (acc.empty() || acc.back() != '/') acc += "/";
    size_t start = 0;
    while (start < remoteRel.size()) {
        size_t slash = remoteRel.find('/', start);
        std::string seg = remoteRel.substr(0, slash == std::string::npos ? std::string::npos : slash);
        acc = rp + seg;
        std::string extra = " -Q " + ShellQuoteD("MKD " + acc) +
                            " -o " + ShellQuoteD(devnull) + " " + ShellQuoteD(url);
        std::string out;
        CurlRun(pr, extra, out);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    (void)err;
    return true;
}

// Rekursif: unggah isi direktori lokal ke folder remote.
static int FtpPushRecursive(const Profile& pr, const std::string& remotePath,
                            const std::string& localDir, int depth, std::string& err) {
    if (depth > 24) return 0;
    std::vector<LocalEntry> entries = ListLocalDir(localDir);
    EnsureDir(localDir);
    int count = 0;
    for (const auto& e : entries) {
        std::string childRemote = remotePath;
        if (childRemote.empty() || childRemote.back() != '/') childRemote += "/";
        childRemote += e.name;
        std::string localChild;
        JoinPath(localDir, e.name, localChild);

        if (e.isDir) {
            FtpMkdir(pr, childRemote, err);
            int r = FtpPushRecursive(pr, childRemote, localChild, depth + 1, err);
            if (r < 0) return -1;
            count += r;
        } else {
            std::string perr;
            if (!FtpPutFile(pr, childRemote, localChild, perr)) { err = perr; continue; }
            count++;
        }
    }
    return count;
}

// ===========================================================================
// Backend SSH / SFTP (via OpenSSH: sftp)
// ===========================================================================
// Deteksi protokol SSH.
static bool IsSsh(const Profile& pr) {
    return pr.protocol == "ssh" || pr.protocol == "sftp";
}

// Opsi umum. Auth key bila ada (non-interaktif); selain itu password
// ditanyakan OpenSSH di terminal (interaktif).
static std::string SshCommon(const Profile& pr) {
    std::string o = " -o StrictHostKeyChecking=accept-new";
    o += " -o ConnectTimeout=15";
    if (pr.port > 0) o += " -p " + std::to_string(pr.port);  // ssh pakai -p
    if (!pr.key.empty()) o += " -i " + ShellQuoteD(pr.key) + " -o BatchMode=yes";
    return o;
}
static std::string SftpCommon(const Profile& pr) {
    std::string o = " -o StrictHostKeyChecking=accept-new";
    o += " -o ConnectTimeout=15";
    if (pr.port > 0) o += " -P " + std::to_string(pr.port);  // sftp pakai -P
    if (!pr.key.empty()) o += " -i " + ShellQuoteD(pr.key) + " -o BatchMode=yes";
    return o;
}

// Target user@host
static std::string SshTarget(const Profile& pr) {
    return pr.user.empty() ? pr.host : (pr.user + "@" + pr.host);
}

// Normalisasi path remote.
static std::string NormalizeRemote(const std::string& rp) {
    if (rp.empty()) return "/";
    std::string r = rp;
    if (r[0] != '/') r = "/" + r;
    while (r.size() > 1 && r.back() == '/') r.pop_back();
    return r;
}

// Gabung folder remote dasar dengan path relatif.
static std::string RemoteJoin(const Profile& pr, const std::string& rel) {
    std::string rp = NormalizeRemote(pr.remotePath);
    if (rel.empty()) return rp;
    return rp == "/" ? ("/" + rel) : (rp + "/" + rel);
}

// Jalankan perintah shell di server levat ssh, tangkap stdout.
static int SshExec(const Profile& pr, const std::string& shellCmd, std::string& out) {
    std::string cmd = "ssh" + SshCommon(pr) + " " + ShellQuoteD(SshTarget(pr)) +
                      " " + ShellQuoteD(shellCmd) + " 2>&1";
    return RunCapture(cmd, out);
}

// Folder induk sebuah path (untuk `lcd`).
static std::string ParentDir(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    if (s == std::string::npos || s == 0) return path;
    return path.substr(0, s);
}

// Jalankan sftp batch (perintah dalam file sementara).
static int SftpBatch(const Profile& pr, const std::string& batchCmds, std::string& out) {
    EnsureDir(ConfigDir());
    std::string tmp;
    JoinPath(ConfigDir(), "sftp_batch.txt", tmp);
    WriteFileText(tmp, batchCmds);
    std::string cmd = "sftp" + SftpCommon(pr) + " -b " + ShellQuoteD(tmp) +
                      " " + ShellQuoteD(SshTarget(pr)) + " 2>&1";
    return RunCapture(cmd, out);
}

// Daftar isi folder remote (nama saja) via `find -maxdepth 1`.
static bool SshList(const Profile& pr, const std::string& remotePath,
                    std::vector<std::string>& names, std::string& err) {
    std::string rp = remotePath.empty() ? "/" : remotePath;
    std::string out;
    int rc = SshExec(pr, "cd " + ShellQuoteD(rp) + " && ls -1a", out);
    if (rc != 0) { err = out; return false; }
    names.clear();
    size_t i = 0;
    while (i < out.size()) {
        size_t e = out.find('\n', i);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        i = e + 1;
        if (line.empty() || line == "." || line == "..") continue;
        names.push_back(line);
    }
    return true;
}

// Unduh satu berkas via sftp batch.
static bool SshGetFile(const Profile& pr, const std::string& remoteRel,
                       const std::string& localFile, std::string& err) {
    std::string full = RemoteJoin(pr, remoteRel);
    std::string cmds = "lcd " + ShellQuoteD(ParentDir(localFile)) + "\n" +
                       "get " + ShellQuoteD(full) + " " + ShellQuoteD(localFile) + "\n";
    std::string out;
    int rc = SftpBatch(pr, cmds, out);
    if (rc != 0) { err = out; return false; }
    return true;
}

// Unggah satu berkas via sftp batch.
static bool SshPutFile(const Profile& pr, const std::string& remoteRel,
                       const std::string& localFile, std::string& err) {
    std::string full = RemoteJoin(pr, remoteRel);
    std::string cmds = "lcd " + ShellQuoteD(ParentDir(localFile)) + "\n" +
                       "put " + ShellQuoteD(localFile) + " " + ShellQuoteD(full) + "\n";
    std::string out;
    int rc = SftpBatch(pr, cmds, out);
    if (rc != 0) { err = out; return false; }
    return true;
}

// Buat folder remote (mkdir -p).
static bool SshMkdir(const Profile& pr, const std::string& remoteRel, std::string& err) {
    std::string full = RemoteJoin(pr, remoteRel);
    std::string out;
    SshExec(pr, "mkdir -p " + ShellQuoteD(full), out);
    (void)err;
    return true;
}

// ===========================================================================
// Mesin sinkron (banding lokal vs remote)
// ===========================================================================
struct RemoteEntry {
    std::string rel;   // path relatif terhadap remotePath
    long long size = 0;
    long long mtime = 0;  // epoch detik (0 = tak diketahui)
    std::string hash;     // SHA-1 (hanya diisi saat --checksum)
};

// Parse tanggal HTTP "Tue, 13 Oct 2026 17:05:10 GMT" -> epoch detik.
static long long ParseHttpDate(const std::string& s) {
    // Lewati "Day, "
    size_t p = s.find(',');
    if (p != std::string::npos) p++; else p = 0;
    while (p < s.size() && s[p] == ' ') p++;
    // "13 Oct 2026 17:05:10 GMT"
    static const char* mon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    int day = 0, year = 0, hour = 0, min = 0, sec = 0, monIdx = -1;
    char monStr[4] = {0};
    if (sscanf(s.c_str() + p, "%d %3s %d %d:%d:%d",
               &day, monStr, &year, &hour, &min, &sec) < 6) return 0;
    for (int i = 0; i < 12; ++i) if (strncmp(monStr, mon[i], 3) == 0) { monIdx = i; break; }
    if (monIdx < 0) return 0;
    struct tm tm{};
    tm.tm_mday = day; tm.tm_mon = monIdx; tm.tm_year = year - 1900;
    tm.tm_hour = hour; tm.tm_min = min; tm.tm_sec = sec;
#ifdef _WIN32
    return (long long)_mkgmtime(&tm);
#else
    return (long long)timegm(&tm);
#endif
}

// Batch HEAD untuk FTP: ambil size+mtime banyak berkas dalam satu koneksi.
// hasil: map rel -> {size, mtime}.
static bool FtpHeadMany(const Profile& pr, const std::vector<std::string>& rels,
                        std::vector<RemoteEntry>& out) {
    if (rels.empty()) return true;
    std::string urlBase = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) urlBase += ":" + std::to_string(pr.port);
    std::string rp = pr.remotePath;
    if (rp.empty() || rp[0] != '/') rp = "/" + rp;
    if (rp.back() != '/') rp += "/";

    // Bangun perintah: -I url1 url2 ... -w "MARK url\n"
    std::string cmd = "curl -sSI";
    if (pr.protocol == "ftps") cmd += " --ssl-reqd";
    if (!pr.user.empty())
        cmd += " --user " + ShellQuoteD(pr.user + ":" + EffectivePassword(pr));
    cmd += " --connect-timeout 15 --max-time 900";
    for (const auto& r : rels) cmd += " " + ShellQuoteD(urlBase + rp + r);
    cmd += " -w " + ShellQuoteD("MARK %{url_effective}\n") + " 2>&1";
    std::string raw;
    RunCapture(cmd, raw);

    // Urutkan hasil berdasarkan kemunculan MARK agar bisa dipetakan ke rel.
    size_t pos = 0;
    for (const auto& r : rels) {
        RemoteEntry e;
        e.rel = r;
        e.size = -1;
        // cari blok sebelum "MARK <url yang berakhiran r>"
        std::string want = "MARK " + urlBase + rp + r;
        size_t mk = raw.find(want, pos);
        if (mk == std::string::npos) { out.push_back(std::move(e)); continue; }
        std::string block = raw.substr(pos, mk - pos);
        pos = mk;
        size_t cl = block.find("Content-Length:");
        if (cl != std::string::npos) {
            cl += 15;
            while (cl < block.size() && (block[cl] == ' ' || block[cl] == '\t')) cl++;
            long long v = 0; bool any = false;
            while (cl < block.size() && isdigit((unsigned char)block[cl])) { v = v*10 + (block[cl]-'0'); cl++; any = true; }
            if (any) e.size = v;
        }
        size_t lmIdx = block.find("Last-Modified:");
        if (lmIdx != std::string::npos) {
            lmIdx += 14;
            while (lmIdx < block.size() && block[lmIdx] == ' ') lmIdx++;
            size_t le = block.find('\n', lmIdx);
            if (le == std::string::npos) le = block.size();
            e.mtime = ParseHttpDate(block.substr(lmIdx, le - lmIdx));
        }
        out.push_back(std::move(e));
    }
    return true;
}

// HEAD ke satu berkas remote -> ukuran + mtime. size=-1 bila tidak ada.
static bool FtpHeadMeta(const Profile& pr, const std::string& remoteRel,
                        long long& size, long long& mtime) {
    size = -1; mtime = 0;
    std::string url = (pr.protocol == "ftps" ? "ftps://" : "ftp://") + pr.host;
    if (pr.port > 0) url += ":" + std::to_string(pr.port);
    std::string rp = pr.remotePath;
    if (rp.empty() || rp[0] != '/') rp = "/" + rp;
    if (rp.back() != '/') rp += "/";
    url += rp + remoteRel;
    std::string out;
    int rc = CurlRun(pr, " -I " + ShellQuoteD(url), out);
    if (rc != 0) return false;
    size_t p = out.find("Content-Length:");
    if (p != std::string::npos) {
        p += 15;
        while (p < out.size() && (out[p] == ' ' || out[p] == '\t')) p++;
        long long v = 0; bool any = false;
        while (p < out.size() && isdigit((unsigned char)out[p])) { v = v * 10 + (out[p] - '0'); p++; any = true; }
        if (any) size = v;
    }
    size_t q = out.find("Last-Modified:");
    if (q != std::string::npos) {
        q += 14;
        while (q < out.size() && out[q] == ' ') q++;
        size_t e = out.find('\r', q);
        if (e == std::string::npos) e = out.find('\n', q);
        if (e == std::string::npos) e = out.size();
        mtime = ParseHttpDate(out.substr(q, e - q));
    }
    return size >= 0;
}

// SSH: stat ukuran + mtime. return false bila tidak ada.
static bool SshHeadMeta(const Profile& pr, const std::string& remoteRel,
                        long long& size, long long& mtime) {
    size = -1; mtime = 0;
    std::string full = RemoteJoin(pr, remoteRel);
    std::string out;
    int rc = SshExec(pr, "stat -c '%s %Y' -- " + ShellQuoteD(full) + " 2>/dev/null || stat -f '%z %m' " + ShellQuoteD(full), out);
    if (rc != 0) return false;
    long long s = -1, m = 0;
    if (sscanf(out.c_str(), "%lld %lld", &s, &m) >= 1) { size = s; mtime = m; return true; }
    return false;
}

// Hash SHA-1 berkas remote. SSH: pakai sha1sum; FTP: unduh ke temp lalu hash.
static std::string RemoteSha1(const Profile& pr, const std::string& rel) {
    if (IsSsh(pr)) {
        std::string full = RemoteJoin(pr, rel);
        std::string out;
        int rc = SshExec(pr, "sha1sum " + ShellQuoteD(full) + " 2>/dev/null || shasum -a 1 " + ShellQuoteD(full), out);
        if (rc != 0) return "";
        std::string h;
        for (char c : out) {
            if (isxdigit((unsigned char)c)) h += (char)tolower(c);
            else if (!h.empty()) break;
        }
        return h.size() == 40 ? h : "";
    }
    // FTP: unduh ke berkas sementara, lalu hash.
    std::string tmp;
    JoinPath(ConfigDir(), "hash_tmp", tmp);
    EnsureDir(ConfigDir());
    std::string err;
    if (!FtpGetFile(pr, rel, tmp, err)) return "";
    std::string h = FileSha1(tmp);
    remove(tmp.c_str());
    return h;
}

// Dispatch transport: SSH vs FTP.
static bool RemoteList(const Profile& pr, const std::string& remotePath,
                       std::vector<std::string>& names, std::string& err) {
    return IsSsh(pr) ? SshList(pr, remotePath, names, err)
                     : FtpList(pr, remotePath, names, err);
}
static bool RemoteHeadMeta(const Profile& pr, const std::string& rel,
                           long long& size, long long& mtime) {
    return IsSsh(pr) ? SshHeadMeta(pr, rel, size, mtime)
                     : FtpHeadMeta(pr, rel, size, mtime);
}
static bool RemoteGet(const Profile& pr, const std::string& rel,
                      const std::string& localFile, std::string& err) {
    return IsSsh(pr) ? SshGetFile(pr, rel, localFile, err)
                     : FtpGetFile(pr, rel, localFile, err);
}
static bool RemotePut(const Profile& pr, const std::string& rel,
                      const std::string& localFile, std::string& err) {
    return IsSsh(pr) ? SshPutFile(pr, rel, localFile, err)
                     : FtpPutFile(pr, rel, localFile, err);
}
static bool RemoteMkdir(const Profile& pr, const std::string& rel, std::string& err) {
    return IsSsh(pr) ? SshMkdir(pr, rel, err) : FtpMkdir(pr, rel, err);
}

// Rekursif pindai remote: nama folder + ukuran/mtime berkas.
static bool ScanRemote(const Profile& pr, const std::string& remotePath,
                       const std::string& relPrefix, int depth,
                       std::vector<RemoteEntry>& out, std::string& err,
                       bool checksum = false) {
    if (depth > 24) return true;
    std::vector<std::string> names;
    if (!RemoteList(pr, remotePath, names, err)) return false;
    for (const auto& name : names) {
        std::string childRemote = remotePath;
        if (childRemote.empty() || childRemote.back() != '/') childRemote += "/";
        childRemote += name;
        std::string rel = relPrefix.empty() ? name : relPrefix + "/" + name;

        std::vector<std::string> sub;
        std::string suberr;
        if (RemoteList(pr, childRemote, sub, suberr)) {
            if (!ScanRemote(pr, childRemote, rel, depth + 1, out, err, checksum)) return false;
        } else {
            RemoteEntry e;
            e.rel = rel;
            e.size = -1;  // diisi belakangan secara batch
            out.push_back(std::move(e));
        }
    }
    return true;
}

// Isi size/mtime (dan hash bila checksum) untuk semua entri hasil ScanRemote.
// FTP: satu koneksi HEAD untuk semua berkas. SSH: per berkas via stat.
static void FillRemoteMeta(const Profile& pr, std::vector<RemoteEntry>& out, bool checksum) {
    if (IsSsh(pr)) {
        for (auto& e : out) {
            RemoteHeadMeta(pr, e.rel, e.size, e.mtime);
            if (checksum) e.hash = RemoteSha1(pr, e.rel);
        }
        return;
    }
    std::vector<std::string> rels;
    rels.reserve(out.size());
    for (const auto& e : out) rels.push_back(e.rel);
    std::vector<RemoteEntry> meta;
    FtpHeadMany(pr, rels, meta);
    for (size_t i = 0; i < out.size() && i < meta.size(); ++i) {
        out[i].size = meta[i].size;
        out[i].mtime = meta[i].mtime;
        if (checksum) out[i].hash = RemoteSha1(pr, out[i].rel);
    }
}

// Cek keamanan path relatif: tolak segmen ".." (traversal) dan path absolut.
static bool SafeRel(const std::string& rel) {
    if (rel.empty()) return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (rel.size() >= 2 && rel[1] == ':') return false;  // drive (Windows)
    // pecah per '/', tolak ".."
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        std::string seg = (slash == std::string::npos) ? rel.substr(start)
                                                       : rel.substr(start, slash - start);
        if (seg == ".." || seg == ".") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

// Pola ignore minimal: cocokkan nama berkas/folder atau akhiran.
static bool IsIgnored(const std::vector<std::string>& patterns, const std::string& rel) {
    // Selalu abaikan berkas internal Nxzilla.
    if (rel == ".nxzilla.json" || rel == ".nxzilla-state.json" ||
        rel == ".nxzilla-cache.json") return true;
    std::string base = rel;
    size_t s = rel.find_last_of('/');
    if (s != std::string::npos) base = rel.substr(s + 1);
    for (const auto& pat : patterns) {
        if (pat.empty()) continue;
        std::string p = pat;
        bool wildcard = false;
        if (!p.empty() && p.back() == '/') p.pop_back();  // "node_modules/" -> "node_modules"
        if (!p.empty() && p[0] == '*') { wildcard = true; p = p.substr(1); }
        if (wildcard) {
            if (base.size() >= p.size() && base.compare(base.size() - p.size(), p.size(), p) == 0)
                return true;
        } else {
            // cocokkan tiap komponen path
            if (base == p) return true;
            std::string needle = "/" + p + "/";
            if (("/" + rel + "/").find(needle) != std::string::npos) return true;
            if (rel.compare(0, p.size() + 1, p + "/") == 0) return true;
        }
    }
    return false;
}

// Rekursif pindai lokal -> map rel -> size+mtime (+hash bila diminta).
static void ScanLocal(const std::string& dir, const std::string& relPrefix,
                      const std::vector<std::string>& ignore, int depth,
                      std::vector<RemoteEntry>& out, bool checksum = false) {
    if (depth > 24) return;
    for (const auto& e : ListLocalDir(dir)) {
        std::string rel = relPrefix.empty() ? e.name : relPrefix + "/" + e.name;
        if (IsIgnored(ignore, rel)) continue;
        std::string full;
        JoinPath(dir, e.name, full);
        if (e.isDir) {
            ScanLocal(full, rel, ignore, depth + 1, out, checksum);
        } else {
            RemoteEntry x;
            x.rel = rel;
            x.size = FileSize(full);
            x.mtime = FileMTime(full);
            if (checksum) x.hash = FileSha1(full);
            out.push_back(std::move(x));
        }
    }
}

struct DiffItem {
    char kind;          // 'A' push-lokal, 'D' tarik-remote, 'M' ubah
    std::string rel;
    long long localSize = -1;
    long long remoteSize = -1;
    bool localNewer = false;   // untuk 'M': sisi mana yang lebih baru
    bool conflict = false;     // 'M': lokal & remote dua-duanya berubah
};

// Toleransi perbedaan waktu (detik) agar tidak salah deteksi karena presisi.
static const long long kMTimeTolerance = 2;

// Baseline: kondisi terakhir yang sudah sinkron (rel -> size+mtime).
using Baseline = std::vector<RemoteEntry>;

static std::string StatePath(const std::string& projectDir) {
    std::string p;
    JoinPath(projectDir, ".nxzilla-state.json", p);
    return p;
}

static Baseline LoadBaseline(const std::string& projectDir) {
    Baseline out;
    std::string raw = ReadFileText(StatePath(projectDir));
    if (raw.empty()) return out;
    JVal root;
    JParser jp(raw);
    if (!jp.parse(root)) return out;
    const JVal* arr = root.arr("files");
    if (!arr) return out;
    for (const JVal& e : arr->items) {
        RemoteEntry x;
        x.rel = e.str("rel");
        const JVal* s = e.find("size");
        const JVal* m = e.find("mtime");
        x.size = (s && s->type == JVal::NUM) ? (long long)s->number : -1;
        x.mtime = (m && m->type == JVal::NUM) ? (long long)m->number : 0;
        x.hash = e.str("hash");
        if (!x.rel.empty()) out.push_back(std::move(x));
    }
    return out;
}

static bool SaveBaseline(const std::string& projectDir, const Baseline& b) {
    std::string j = "{\n  \"files\": [\n";
    for (size_t i = 0; i < b.size(); ++i) {
        const RemoteEntry& e = b[i];
        j += "    { \"rel\": \"" + EscapeJson(e.rel) + "\", \"size\": " +
             std::to_string(e.size) + ", \"mtime\": " + std::to_string(e.mtime) +
             ", \"hash\": \"" + EscapeJson(e.hash) + "\" }";
        if (i + 1 < b.size()) j += ",";
        j += "\n";
    }
    j += "  ]\n}\n";
    return WriteFileText(StatePath(projectDir), j);
}

// ===========================================================================
// Cache metadata remote (dipakai status/watch agar tidak selalu LIST+HEAD).
// ===========================================================================
struct RemoteCache {
    long long at = 0;                     // waktu scan (epoch detik)
    std::vector<RemoteEntry> files;       // rel -> size/mtime
};

static std::string CachePath(const std::string& projectDir) {
    std::string p;
    JoinPath(projectDir, ".nxzilla-cache.json", p);
    return p;
}

static RemoteCache LoadCache(const std::string& projectDir) {
    RemoteCache c;
    std::string raw = ReadFileText(CachePath(projectDir));
    if (raw.empty()) return c;
    JVal root;
    JParser jp(raw);
    if (!jp.parse(root)) return c;
    const JVal* at = root.find("at");
    c.at = (at && at->type == JVal::NUM) ? (long long)at->number : 0;
    const JVal* arr = root.arr("files");
    if (!arr) return c;
    for (const JVal& e : arr->items) {
        RemoteEntry x;
        x.rel = e.str("rel");
        const JVal* s = e.find("size");
        const JVal* m = e.find("mtime");
        x.size = (s && s->type == JVal::NUM) ? (long long)s->number : -1;
        x.mtime = (m && m->type == JVal::NUM) ? (long long)m->number : 0;
        if (!x.rel.empty()) c.files.push_back(std::move(x));
    }
    return c;
}

static void SaveCache(const std::string& projectDir, const std::vector<RemoteEntry>& files) {
    std::string j = "{\n  \"at\": " + std::to_string((long long)time(nullptr)) +
                    ",\n  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const RemoteEntry& e = files[i];
        j += "    { \"rel\": \"" + EscapeJson(e.rel) + "\", \"size\": " +
             std::to_string(e.size) + ", \"mtime\": " + std::to_string(e.mtime) + " }";
        if (i + 1 < files.size()) j += ",";
        j += "\n";
    }
    j += "  ]\n}\n";
    WriteFileText(CachePath(projectDir), j);
}

// Banding peta lokal vs remote. kind: 'A' hanya lokal, 'D' hanya remote, 'M' beda.
// `base` (bila ada) menentukan konflik: lokal & remote dua-duanya menyimpang
// dari kondisi terakhir yang sinkron.
static std::vector<DiffItem> DiffTrees(const std::vector<RemoteEntry>& local,
                                       const std::vector<RemoteEntry>& remote,
                                       const Baseline& base, bool checksum) {
    std::vector<DiffItem> out;
    auto find = [](const std::vector<RemoteEntry>& v, const std::string& rel) -> const RemoteEntry* {
        for (const auto& e : v) if (e.rel == rel) return &e;
        return nullptr;
    };
    for (const auto& le : local) {
        const RemoteEntry* re = find(remote, le.rel);
        if (!re) {
            out.push_back({'A', le.rel, le.size, -1, true, false});
            continue;
        }
        bool sizeDiff = (re->size >= 0 && le.size >= 0 && re->size != le.size);
        bool timeDiff = false;
        if (!sizeDiff && re->mtime > 0 && le.mtime > 0) {
            long long d = le.mtime - re->mtime;
            if (d > kMTimeTolerance || d < -kMTimeTolerance) timeDiff = true;
        }
        bool changed = sizeDiff || timeDiff;
        // Mode checksum: banding hash apa pun size/mtime-nya.
        if (checksum && !le.hash.empty() && !re->hash.empty()) {
            changed = (le.hash != re->hash);
        }
        if (changed) {
            DiffItem it;
            it.kind = 'M';
            it.rel = le.rel;
            it.localSize = le.size;
            it.remoteSize = re->size;
            it.localNewer = (le.mtime >= re->mtime);
            // Konflik: ada baseline, dan lokal maupun remote menyimpang dari itu.
            const RemoteEntry* be = find(base, le.rel);
            if (be) {
                bool hOk = checksum && !be->hash.empty();
                bool localChanged = hOk ? (be->hash != le.hash)
                                        : ((be->size != le.size) ||
                                           (be->mtime > 0 && le.mtime > 0 &&
                                            (le.mtime - be->mtime > kMTimeTolerance ||
                                             be->mtime - le.mtime > kMTimeTolerance)));
                bool remoteChanged = hOk ? (be->hash != re->hash)
                                         : ((be->size != re->size) ||
                                            (be->mtime > 0 && re->mtime > 0 &&
                                             (re->mtime - be->mtime > kMTimeTolerance ||
                                              be->mtime - re->mtime > kMTimeTolerance)));
                it.conflict = localChanged && remoteChanged;
            }
            out.push_back(it);
        }
    }
    for (const auto& re : remote) {
        if (!find(local, re.rel)) out.push_back({'D', re.rel, -1, re.size, false, false});
    }
    std::sort(out.begin(), out.end(),
              [](const DiffItem& a, const DiffItem& b) { return a.rel < b.rel; });
    return out;
}

// Ambil metadata remote, pakai cache bila masih segar (dan bukan checksum).
// Mengembalikan true bila remote berhasil disediakan.
static bool GetRemoteEntries(const Profile& pr, const std::string& remotePath,
                             const std::string& localDir, bool checksum,
                             int cacheTtl, std::vector<RemoteEntry>& remote,
                             std::string& err) {
    if (!checksum && cacheTtl > 0) {
        RemoteCache c = LoadCache(localDir);
        long long now = (long long)time(nullptr);
        if (c.at > 0 && (now - c.at) <= cacheTtl && !c.files.empty()) {
            remote = c.files;
            return true;
        }
    }
    if (!ScanRemote(pr, remotePath, "", 0, remote, err, checksum)) return false;
    FillRemoteMeta(pr, remote, checksum);
    if (!checksum) SaveCache(localDir, remote);
    return true;
}

// Susun rencana sinkron (dipakai status/pull/push).
// cacheTtl > 0: izinkan pakai cache metadata remote segar (mis. status/watch).
static bool BuildPlan(const Profile& pr, const std::string& remotePath,
                      const std::string& localDir,
                      const std::vector<std::string>& ignore,
                      std::vector<DiffItem>& plan, std::string& err,
                      bool checksum = false, int cacheTtl = 0) {
    std::vector<RemoteEntry> local, remote;
    ScanLocal(localDir, "", ignore, 0, local, checksum);
    if (!GetRemoteEntries(pr, remotePath, localDir, checksum, cacheTtl, remote, err)) return false;
    Baseline base = LoadBaseline(localDir);
    plan = DiffTrees(local, remote, base, checksum);
    return true;
}

// Setelah sync sukses: simpan kondisi remote (yang kini sama dgn lokal) sebagai
// baseline, agar konflik berikutnya bisa dideteksi.
static void SaveSyncedBaseline(const Profile& pr, const std::string& remotePath,
                               const std::string& localDir) {
    std::vector<RemoteEntry> remote;
    std::string err;
    if (ScanRemote(pr, remotePath, "", 0, remote, err)) {
        FillRemoteMeta(pr, remote, false);
        std::sort(remote.begin(), remote.end(),
                  [](const RemoteEntry& a, const RemoteEntry& b) { return a.rel < b.rel; });
        SaveBaseline(localDir, remote);
        SaveCache(localDir, remote);  // cache ikut ter-refresh
    }
}

// ===========================================================================
// Utilitas tampilan
// ===========================================================================
// Diff baris sederhana (LCS). Menghasilkan teks unified-like.
static std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) { if (i < s.size()) out.push_back(s.substr(i)); break; }
        std::string line = s.substr(i, e - i);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        i = e + 1;
    }
    return out;
}

// Tampilkan perbedaan dua berkas. return jumlah baris berbeda.
static int PrintDiff(const std::string& a, const std::string& b,
                     const std::string& labelA, const std::string& labelB) {
    std::vector<std::string> A = SplitLines(a), B = SplitLines(b);
    size_t n = A.size(), m = B.size();
    // LCS table
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            dp[i][j] = (A[i] == B[j]) ? dp[i+1][j+1] + 1
                                      : (std::max)(dp[i+1][j], dp[i][j+1]);
    int changes = 0;
    size_t i = 0, j = 0;
    bool headerShown = false;
    auto header = [&]() {
        if (!headerShown) { Out("--- " + labelA + "\n+++ " + labelB + "\n"); headerShown = true; }
    };
    while (i < n && j < m) {
        if (A[i] == B[j]) { i++; j++; }
        else if (dp[i+1][j] >= dp[i][j+1]) { header(); Out("\x1b[1;31m-" + A[i] + "\x1b[0m\n"); i++; changes++; }
        else { header(); Out("\x1b[1;32m+" + B[j] + "\x1b[0m\n"); j++; changes++; }
    }
    while (i < n) { header(); Out("\x1b[1;31m-" + A[i] + "\x1b[0m\n"); i++; changes++; }
    while (j < m) { header(); Out("\x1b[1;32m+" + B[j] + "\x1b[0m\n"); j++; changes++; }
    return changes;
}

// Format ukuran byte -> "1.2 KB", "3.4 MB".
static std::string HumanBytes(long long b) {
    if (b < 1024) return std::to_string(b) + " B";
    double v = (double)b;
    const char* u[] = {"KB", "MB", "GB", "TB"};
    int i = -1;
    while (v >= 1024 && i < 3) { v /= 1024; i++; }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", v, u[i < 0 ? 0 : i]);
    return buf;
}

// Ringkasan transfer: "3 file(s), 12.4 KB in 1.2s (10.3 KB/s)".
static std::string TransferSummary(const std::string& verb, int files, long long bytes,
                                   double seconds) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s %d file(s), %s", verb.c_str(), files,
             HumanBytes(bytes).c_str());
    std::string s = buf;
    if (seconds > 0.0) {
        snprintf(buf, sizeof(buf), " in %.1fs (", seconds);
        s += buf;
        double bps = (double)bytes / seconds;
        s += HumanBytes((long long)bps) + "/s)";
    }
    return s;
}

static std::string PadClip(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return s + std::string(width - s.size(), ' ');
}

static std::string OkBadge(const std::string& status) {
    if (status == "synced") return "\x1b[1;32m";
    if (status == "dirty")  return "\x1b[1;33m";
    if (status == "error")  return "\x1b[1;31m";
    return "\x1b[90m";
}

// ===========================================================================
// TUI screens
// ===========================================================================
enum Screen { SC_GLOBAL, SC_PROJECT, SC_PROFILES, SC_FORM, SC_ONBOARD };
enum FormKind { FORM_ADD_PROJECT, FORM_EDIT_PROJECT, FORM_ADD_PROFILE, FORM_EDIT_PROFILE };

struct Form {
    int kind = FORM_ADD_PROJECT;
    int step = 0;
    int editIndex = -1;
    int returnScreen = SC_GLOBAL;
    std::string buf;
    std::vector<std::string> labels;
    std::vector<std::string> values;
};

struct AppState {
    int screen = SC_GLOBAL;
    int globalSel = 0;
    int profileSel = 0;
    std::string statusMsg = "Ready.";
    std::vector<Project> projects;
    std::vector<Profile> profiles;
    std::string projectRoot;  // when SC_PROJECT
    std::string onboardDir;   // when SC_ONBOARD
    Form form;
};

static void Reload(AppState& st) {
    st.projects = LoadProjects();
    st.profiles = LoadProfiles();
}

static const Profile* FindProfile(const std::vector<Profile>& ps, const std::string& name) {
    for (const auto& p : ps) if (p.name == name) return &p;
    return nullptr;
}

static std::string Header(const std::string& title, const char* timeBuf) {
    return std::string(kOrange) + "  NXZILLA" + kReset + "\x1b[1m  " + title + kReset +
           "   " + kGray + timeBuf + kReset + "\x1b[K\r\n";
}

static std::string StatusLine(const std::string& msg) {
    return std::string(kStatus) + " STATUS " + kReset + " " + msg + "\x1b[K\r\n";
}

static void TimeNow(char* buf, size_t n) {
    time_t t = time(nullptr);
    tm* lt = localtime(&t);
    if (lt) strftime(buf, n, "%H:%M:%S", lt);
    else buf[0] = 0;
}

// ---- Form helpers ----
static void StartForm(AppState& st, int kind, int editIndex) {
    Form& fm = st.form;
    fm.kind = kind;
    fm.editIndex = editIndex;
    fm.step = 0;

    if (st.profiles.empty()) st.profiles = LoadProfiles();
    if (st.projects.empty()) st.projects = LoadProjects();

    if (kind == FORM_ADD_PROJECT) {
        // One-step add: project info + connection credentials together.
        fm.labels = {"Name", "Local folder", "Protocol (ftp/ftps/ssh)", "Host", "Port",
                     "User", "Password", "Remote path"};
        fm.values = {"", CurrentDir(), "ftp", "", "21", "", "", ""};
    } else if (kind == FORM_EDIT_PROJECT) {
        fm.labels = {"Name", "Local folder", "Profile", "Remote path"};
        if (editIndex >= 0 && editIndex < (int)st.projects.size()) {
            const Project& p = st.projects[editIndex];
            fm.values = {p.name, p.localPath, p.profile, p.remotePath};
        } else {
            fm.values = {"", CurrentDir(), st.profiles.empty() ? "" : st.profiles[0].name, ""};
        }
    } else {
        fm.labels = {"Name", "Protocol (ftp/ftps/ssh)", "Host", "Port",
                     "User", "Password", "Remote path"};
        if (kind == FORM_EDIT_PROFILE && editIndex >= 0 &&
            editIndex < (int)st.profiles.size()) {
            const Profile& p = st.profiles[editIndex];
            std::string pw = p.password;
            if (pw.empty()) pw = KeyringGet(p.name);  // dari keyring OS
            fm.values = {p.name, p.protocol, p.host, std::to_string(p.port),
                         p.user, pw, p.remotePath};
        } else {
            fm.values = {"", "ftp", "", "21", "", "", ""};
        }
    }
    fm.buf = fm.values.empty() ? "" : fm.values[0];
    st.screen = SC_FORM;
}

static void CommitForm(AppState& st) {
    Form& fm = st.form;
    if (fm.kind == FORM_ADD_PROJECT) {
        // Build profile from the connection fields entered in the same form.
        std::string pname = fm.values.size() > 3 && !fm.values[3].empty() ? fm.values[3]
                                                                            : "(unnamed)";
        Profile prof;
        prof.name = pname;
        prof.protocol = fm.values.size() > 2 && !fm.values[2].empty() ? fm.values[2] : "ftp";
        prof.host = fm.values.size() > 3 ? fm.values[3] : "";
        prof.port = fm.values.size() > 4 ? atoi(fm.values[4].c_str()) : 21;
        prof.user = fm.values.size() > 5 ? fm.values[5] : "";
        prof.password = fm.values.size() > 6 ? fm.values[6] : "";
        prof.remotePath = fm.values.size() > 7 ? fm.values[7] : "";
        bool haveProfile = false;
        for (auto& p : st.profiles) {
            if (p.name == prof.name) { p = prof; haveProfile = true; break; }
        }
        if (!haveProfile) st.profiles.push_back(prof);
        SaveProfiles(st.profiles);

        Project p;
        p.name = fm.values.size() > 0 && !fm.values[0].empty() ? fm.values[0] : "(unnamed)";
        p.localPath = fm.values.size() > 1 ? fm.values[1] : "";
        p.profile = prof.name;
        p.remotePath = prof.remotePath;
        p.lastStatus = "unknown";
        st.projects.push_back(p);
        SaveProjects(st.projects);
        st.statusMsg = "\x1b[1;32m[OK]\x1b[0m Project saved (profile '" + prof.name + "').";
        st.screen = fm.returnScreen;
        Reload(st);
        return;
    }
    if (fm.kind == FORM_EDIT_PROJECT) {
        Project p;
        p.name = fm.values.size() > 0 && !fm.values[0].empty() ? fm.values[0] : "(unnamed)";
        p.localPath = fm.values.size() > 1 ? fm.values[1] : "";
        p.profile = fm.values.size() > 2 ? fm.values[2] : "";
        p.remotePath = fm.values.size() > 3 ? fm.values[3] : "";
        if (fm.kind == FORM_EDIT_PROJECT && fm.editIndex >= 0 &&
            fm.editIndex < (int)st.projects.size()) {
            p.lastSync = st.projects[fm.editIndex].lastSync;
            p.lastStatus = st.projects[fm.editIndex].lastStatus;
            st.projects[fm.editIndex] = p;
        } else {
            p.lastStatus = "unknown";
            st.projects.push_back(p);
        }
        SaveProjects(st.projects);
        st.statusMsg = "\x1b[1;32m[OK]\x1b[0m Project saved.";
    } else {
        Profile p;
        p.name = fm.values.size() > 0 && !fm.values[0].empty() ? fm.values[0] : "(unnamed)";
        p.protocol = fm.values.size() > 1 && !fm.values[1].empty() ? fm.values[1] : "ftp";
        p.host = fm.values.size() > 2 ? fm.values[2] : "";
        p.port = fm.values.size() > 3 ? atoi(fm.values[3].c_str()) : 0;
        p.user = fm.values.size() > 4 ? fm.values[4] : "";
        p.password = fm.values.size() > 5 ? fm.values[5] : "";
        p.remotePath = fm.values.size() > 6 ? fm.values[6] : "";
        if (fm.kind == FORM_EDIT_PROFILE && fm.editIndex >= 0 &&
            fm.editIndex < (int)st.profiles.size()) {
            st.profiles[fm.editIndex] = p;
        } else {
            st.profiles.push_back(p);
        }
        SaveProfiles(st.profiles);
        st.statusMsg = "\x1b[1;32m[OK]\x1b[0m Profile saved.";
    }
    st.screen = fm.returnScreen;
    Reload(st);
}

// ---- Global screen: project list ----
static void RenderGlobal(const AppState& st, int termRows, int termCols, std::string& f) {
    (void)termCols;
    char tb[64];
    TimeNow(tb, sizeof(tb));

    f += Header("Global  (projects)", tb);
    f += "\x1b[K\r\n";
    f += "  Connection profiles: \x1b[1m" + std::to_string(st.profiles.size()) + kReset +
         "      Registered projects: \x1b[1m" + std::to_string(st.projects.size()) + kReset +
         "\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += "\x1b[1m  Projects:\x1b[0m\x1b[K\r\n";

    int visible = termRows - 9;
    if (visible < 3) visible = 3;
    int total = (int)st.projects.size();
    int top = 0;
    if (total > visible) {
        top = st.globalSel - visible / 2;
        if (top < 0) top = 0;
        if (top > total - visible) top = total - visible;
    }

    if (total == 0) {
        f += std::string(kGray) +
             "    (no projects yet) - press [a] to add, or run nxzilla inside a project folder"
             "\x1b[0m\x1b[K\r\n";
    } else {
        for (int i = top; i < total && i < top + visible; ++i) {
            const Project& p = st.projects[i];
            bool sel = (i == st.globalSel);
            std::string st2 = p.lastStatus.empty() ? "unknown" : p.lastStatus;
            std::string line = std::string(sel ? "  > " : "    ") +
                               PadClip(p.name, 18) + PadClip(p.localPath, 30) +
                               PadClip(p.profile, 14) +
                               std::string(OkBadge(st2)) + PadClip(st2, 9) + kReset;
            if (sel) f += std::string(kSel) + line + kReset + "\x1b[K\r\n";
            else f += line + "\x1b[K\r\n";
        }
    }
    f += "\x1b[K\r\n";
    f += std::string(kGray) +
         "  [\u2191/\u2193] select  [Enter] open  [e] edit  [a] add  [d] delete  [p] push  "
         "[l] pull  [s] status  [c] profiles  [r] refresh  [q] quit\x1b[0m\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += StatusLine(st.statusMsg);
}

// ---- Onboarding screen ----
static void RenderOnboard(const AppState& st, std::string& f) {
    char tb[64];
    TimeNow(tb, sizeof(tb));
    f += Header("Setup", tb);
    f += "\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += "  This folder is not a Nxzilla project yet:\x1b[K\r\n";
    f += "    \x1b[1m" + st.onboardDir + kReset + "\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += "  Register it and pull the remote project into this folder?\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += std::string(kGray) +
         "  [Enter/Y] yes, set up now   [n] no, open global space   [q] quit\x1b[0m\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += StatusLine(st.statusMsg);
}

// ---- Project screen ----
static void RenderProject(const AppState& st, std::string& f) {
    char tb[64];
    TimeNow(tb, sizeof(tb));

    Project cur;
    for (const auto& p : st.projects) {
        if (p.localPath == st.projectRoot) { cur = p; break; }
    }

    f += Header("Project: " + (cur.name.empty() ? st.projectRoot : cur.name), tb);
    f += "\x1b[K\r\n";
    f += "  Local   : \x1b[1m" +
         std::string(st.projectRoot.empty() ? CurrentDir() : st.projectRoot) + kReset + "\x1b[K\r\n";
    f += "  Remote  : \x1b[1m" + (cur.remotePath.empty() ? "(not set)" : cur.remotePath) +
         kReset + "\x1b[K\r\n";

    if (!cur.profile.empty()) {
        const Profile* pf = FindProfile(st.profiles, cur.profile);
        std::string det = cur.profile;
        if (pf) {
            det = pf->protocol + " " + pf->user + "@" + pf->host;
            if (pf->port) det += ":" + std::to_string(pf->port);
            det += "   (" + cur.profile + ")";
        }
        f += "  Profile : \x1b[1m" + det + kReset + "\x1b[K\r\n";
    } else {
        f += "  Profile : " + std::string(kGray) + "(not set)" + kReset + "\x1b[K\r\n";
    }
    std::string st2 = cur.lastStatus.empty() ? "unknown" : cur.lastStatus;
    f += std::string("  Status  : ") + OkBadge(st2) + st2 + kReset +
         (cur.lastSync.empty() ? "" : std::string("   ") + kGray + cur.lastSync + kReset) +
         "\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += std::string(kGray) +
         "  [p] push  [l] pull  [s] status  [e] edit  [g] global  [c] profiles  [q] quit"
         "\x1b[0m\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += StatusLine(st.statusMsg);
}

// ---- Profiles / settings screen ----
static void RenderProfiles(const AppState& st, std::string& f) {
    char tb[64];
    TimeNow(tb, sizeof(tb));

    f += Header("Settings  (connection profiles)", tb);
    f += "\x1b[K\r\n";
    if (st.profiles.empty()) {
        f += std::string(kGray) +
             "  (no profiles yet) - press [a] to add one\x1b[0m\x1b[K\r\n";
    } else {
        f += std::string(kGray) + "    " + PadClip("NAME", 16) + PadClip("PROTO", 7) +
             PadClip("HOST", 24) + PadClip("USER", 14) + "REMOTE PATH" + kReset + "\x1b[K\r\n";
        for (size_t i = 0; i < st.profiles.size(); ++i) {
            const Profile& p = st.profiles[i];
            bool sel = ((int)i == st.profileSel);
            std::string host = p.host + (p.port ? ":" + std::to_string(p.port) : "");
            std::string line = std::string(sel ? "  > " : "    ") + PadClip(p.name, 16) +
                               PadClip(p.protocol, 7) + PadClip(host, 24) +
                               PadClip(p.user, 14) + PadClip(p.remotePath, 22);
            if (sel) f += std::string(kSel) + line + kReset + "\x1b[K\r\n";
            else f += line + "\x1b[K\r\n";
        }
    }
    f += "\x1b[K\r\n";
    f += std::string(kGray) +
         "  [\u2191/\u2193] select  [Enter/e] edit  [a] add  [d] delete  [Esc] back\x1b[0m\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += StatusLine(st.statusMsg);
}

// ---- Generic form screen ----
static void RenderForm(const AppState& st, std::string& f) {
    char tb[64];
    TimeNow(tb, sizeof(tb));

    const char* title = "Form";
    switch (st.form.kind) {
        case FORM_ADD_PROJECT:  title = "Add project"; break;
        case FORM_EDIT_PROJECT: title = "Edit project"; break;
        case FORM_ADD_PROFILE:  title = "Add profile"; break;
        case FORM_EDIT_PROFILE: title = "Edit profile"; break;
    }
    f += Header(title, tb);
    f += "\x1b[K\r\n";
    for (size_t i = 0; i < st.form.labels.size(); ++i) {
        bool cur = ((int)i == st.form.step);
        bool isPassword = (st.form.labels[i] == "Password");
        std::string val;
        if (cur) {
            val = st.form.buf;
        } else {
            val = (i < st.form.values.size()) ? st.form.values[i] : "";
            if (isPassword && !val.empty()) val = std::string(val.size(), '*');
        }
        std::string line = std::string(cur ? "  > " : "    ") +
                           PadClip(st.form.labels[i], 22) + ": ";
        if (cur) line += "\x1b[1m" + val + "_\x1b[0m";
        else line += val;
        f += line + "\x1b[K\r\n";
    }
    f += "\x1b[K\r\n";
    f += std::string(kGray) +
         "  [Enter] next/save  [Backspace] delete  [Esc] cancel\x1b[0m\x1b[K\r\n";
    f += "\x1b[K\r\n";
    f += StatusLine(st.statusMsg);
}

// ===========================================================================
// TUI loop
// ===========================================================================
// Render satu frame sekarang (dipakai saat transfer progress).
static void TuiRepaint(const AppState& st) {
    int rows = 30, cols = 100;
    GetTerminalSize(rows, cols);
    std::string f;
    f.reserve(1 << 14);
    f += "\x1b[?2026h";
    f += "\x1b[H";
    if (st.screen == SC_GLOBAL) RenderGlobal(st, rows, cols, f);
    else if (st.screen == SC_PROJECT) RenderProject(st, f);
    else if (st.screen == SC_PROFILES) RenderProfiles(st, f);
    else if (st.screen == SC_ONBOARD) RenderOnboard(st, f);
    else RenderForm(st, f);
    f += "\x1b[?2026l";
    Out(f);
    fflush(stdout);
}

// Jalankan pull/push untuk sebuah entri proyek. Mengembalikan pesan status.
static std::string TuiSync(AppState& st, const Project& proj, bool isPush) {
    const Profile* pr = FindProfile(st.profiles, proj.profile);
    if (!pr) return "\x1b[1;31m[!]\x1b[0m Profile '" + proj.profile + "' not found.";
    std::string effectivePath = proj.remotePath.empty() ? pr->remotePath : proj.remotePath;
    Profile use = *pr;
    use.remotePath = effectivePath;

    std::vector<std::string> ignore = LoadProjectIgnore(proj.localPath);
    std::vector<DiffItem> plan;
    std::string err;
    if (!BuildPlan(use, effectivePath, proj.localPath, ignore, plan, err))
        return "\x1b[1;31m[!]\x1b[0m Cannot scan remote: " + err;

    auto tStart = std::chrono::steady_clock::now();
    long long bytes = 0;
    int done = 0, skipped = 0, conflicts = 0;
    // Hitung total berkas yang akan ditransfer.
    int totalWork = 0;
    for (const auto& it : plan) {
        if (isPush && it.kind == 'D') continue;
        if (!isPush && it.kind == 'A') continue;
        if (it.conflict) continue;  // konflik dilewati di TUI (pakai CLI --force)
        totalWork++;
    }
    std::string verb = isPush ? "Pushing" : "Pulling";
    int step = 0;
    for (const auto& it : plan) {
        if (isPush && it.kind == 'D') { skipped++; continue; }
        if (!isPush && it.kind == 'A') { skipped++; continue; }
        if (it.conflict) { conflicts++; continue; }  // TUI: tahan konflik
        step++;
        // Tampilkan progress segera sebelum berkas ini diproses.
        st.statusMsg = std::string(kOrange) + verb + " " + kReset +
                       "[" + std::to_string(step) + "/" + std::to_string(totalWork) + "] " +
                       it.rel;
        TuiRepaint(st);
        std::string localFile = proj.localPath;
        if (localFile.back() != '/' && localFile.back() != '\\') localFile += kPathSep;
        std::string rel = it.rel;
        if (!SafeRel(rel)) { skipped++; continue; }  // tolak traversal
        for (char& c : rel) if (c == '/') c = kPathSep;
        localFile += rel;
        std::string e2;
        if (isPush) {
            size_t slash = it.rel.find_last_of('/');
            if (slash != std::string::npos) {
                std::string derr;
                RemoteMkdir(use, it.rel.substr(0, slash), derr);
            }
            if (RemotePut(use, it.rel, localFile, e2)) {
                // Selaraskan mtime lokal dgn remote (yang baru di-set ke now).
                long long rs = 0, rm = 0;
                if (RemoteHeadMeta(use, it.rel, rs, rm) && rm > 0) SetFileMTime(localFile, rm);
                if (it.localSize > 0) bytes += it.localSize;
                done++;
            }
        } else {
            size_t slash = localFile.find_last_of("/\\");
            if (slash != std::string::npos) EnsureDir(localFile.substr(0, slash));
            if (RemoteGet(use, it.rel, localFile, e2)) {
                long long rs = 0, rm = 0;
                if (RemoteHeadMeta(use, it.rel, rs, rm) && rm > 0) SetFileMTime(localFile, rm);
                long long sz = FileSize(localFile);
                if (sz > 0) bytes += sz;
                done++;
            }
        }
    }
    // Perbarui registry.
    for (auto& p : st.projects) if (p.name == proj.name) {
        p.lastStatus = "synced";
        char tb[64]; time_t t = time(nullptr); tm* lt = localtime(&t);
        if (lt) strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", lt); else tb[0] = 0;
        p.lastSync = tb;
    }
    SaveProjects(st.projects);
    SaveSyncedBaseline(use, effectivePath, proj.localPath);

    std::string doneverb = isPush ? "Pushed" : "Pulled";
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tStart).count();
    std::string msg = std::string("\x1b[1;32m[OK]\x1b[0m ") +
                      TransferSummary(doneverb, done, bytes, elapsed);
    if (skipped) msg += ", " + std::to_string(skipped) + " skipped";
    if (conflicts) msg += "\x1b[1;31m, " + std::to_string(conflicts) + " CONFLICT (use CLI --force)";
    return msg + ".";
}

// Hitung ringkasan perubahan (untuk [s] status).
static std::string TuiStatus(const AppState& st, const Project& proj) {
    const Profile* pr = FindProfile(st.profiles, proj.profile);
    if (!pr) return "\x1b[1;31m[!]\x1b[0m Profile '" + proj.profile + "' not found.";
    std::string effectivePath = proj.remotePath.empty() ? pr->remotePath : proj.remotePath;
    Profile use = *pr;
    use.remotePath = effectivePath;
    std::vector<std::string> ignore = LoadProjectIgnore(proj.localPath);
    std::vector<DiffItem> plan;
    std::string err;
    if (!BuildPlan(use, effectivePath, proj.localPath, ignore, plan, err))
        return "\x1b[1;31m[!]\x1b[0m Cannot scan remote: " + err;
    if (plan.empty()) return "\x1b[1;32m[synced]\x1b[0m local and remote match.";
    int aN = 0, dN = 0, mN = 0, cN = 0;
    for (const auto& it : plan) {
        if (it.kind == 'A') aN++;
        else if (it.kind == 'D') dN++;
        else { mN++; if (it.conflict) cN++; }
    }
    std::string s = std::to_string(plan.size()) + " change(s): " + std::to_string(aN) +
                    " local-only, " + std::to_string(dN) + " remote-only, " +
                    std::to_string(mN) + " modified";
    if (cN) s += "\x1b[1;31m, " + std::to_string(cN) + " CONFLICT";
    return s + ".";
}

static int RunTui(AppState& st, bool resetStatus = true) {
    RawTerminalScope raw;
    Out("\x1b[?25l");
    Out("\x1b[2J\x1b[H");

    Reload(st);
    if (resetStatus) {
        st.statusMsg = (st.screen == SC_GLOBAL) ? "Ready. Select a project or press [a]."
                                                : "Project detected.";
    }

    bool running = true;
    bool dirty = true;

    // Dev-only: env NXZILLA_TUI_SCRIPT=<keys> menjalankan TUI non-interaktif.
    const char* script = getenv("NXZILLA_TUI_SCRIPT");
    int scriptPos = 0;

    while (running) {
        if (dirty) {
            int rows = 30, cols = 100;
            GetTerminalSize(rows, cols);
            std::string f;
            f.reserve(1 << 14);
            f += "\x1b[?2026h";  // begin synchronized update
            f += "\x1b[H";       // cursor home (no full clear)

            if (st.screen == SC_GLOBAL) RenderGlobal(st, rows, cols, f);
            else if (st.screen == SC_PROJECT) RenderProject(st, f);
            else if (st.screen == SC_PROFILES) RenderProfiles(st, f);
            else if (st.screen == SC_ONBOARD) RenderOnboard(st, f);
            else RenderForm(st, f);

            f += "\x1b[?2026l";  // end synchronized update
            Out(f);
            fflush(stdout);
            dirty = false;
        }

        Key k;
        if (script) {
            if (scriptPos < (int)strlen(script)) {
                unsigned char ch = (unsigned char)script[scriptPos++];
                g_rawInput = (st.screen == SC_FORM);
                if (ch == '\r' || ch == '\n') { k = KEY_ENTER; }
                else if (ch == 27) { k = KEY_ESC; }
                else if (ch == 8 || ch == 127) { k = KEY_BACKSPACE; }
                else { g_lastKeyChar = ch; k = KEY_CHAR; }
            } else {
                FILE* lg = fopen("D:\\CLI\\Nxzilla\\tui_script.log", "a");
                if (lg) { fprintf(lg, "STATUS: %s\n", st.statusMsg.c_str()); fclose(lg); }
                break;
            }
        } else {
            g_rawInput = (st.screen == SC_FORM);
            k = ReadKey(120);
            if (k == KEY_NONE) continue;
        }

        // ---- Onboarding ----
        if (st.screen == SC_ONBOARD) {
            bool yes = (k == KEY_ENTER) ||
                       (k == KEY_CHAR && (g_lastKeyChar == 'y' || g_lastKeyChar == 'Y'));
            bool no = (k == KEY_CHAR && (g_lastKeyChar == 'n' || g_lastKeyChar == 'N'));
            if (yes) {
                // Buka form Add-project, prefilled folder kini, kembali ke global.
                StartForm(st, FORM_ADD_PROJECT, -1);
                st.form.values[1] = st.onboardDir;
                st.form.returnScreen = SC_GLOBAL;
                st.statusMsg = "Editing " + st.form.labels[0];
                dirty = true;
            } else if (no || k == KEY_ESC) {
                st.screen = SC_GLOBAL;
                st.statusMsg = "Ready. Select a project or press [a].";
                dirty = true;
            }
            continue;
        }

        // ---- Form input ----
        if (st.screen == SC_FORM) {
            Form& fm = st.form;
            if (k == KEY_ESC) {
                st.screen = fm.returnScreen;
                st.statusMsg = "Cancelled.";
                dirty = true;
            } else if (k == KEY_BACKSPACE) {
                if (!fm.buf.empty()) fm.buf.pop_back();
                dirty = true;
            } else if (k == KEY_CHAR) {
                fm.buf += (char)g_lastKeyChar;
                dirty = true;
            } else if (k == KEY_ENTER) {
                if (fm.step < (int)fm.values.size()) fm.values[fm.step] = fm.buf;
                if (fm.step + 1 < (int)fm.values.size()) {
                    fm.step++;
                    fm.buf = fm.values[fm.step];
                    st.statusMsg = "Editing " + fm.labels[fm.step];
                } else {
                    CommitForm(st);
                }
                dirty = true;
            }
            continue;
        }

        // ---- Global ----
        if (st.screen == SC_GLOBAL) {
            int total = (int)st.projects.size();
            if (k == KEY_UP && total) { st.globalSel = (st.globalSel - 1 + total) % total; dirty = true; }
            else if (k == KEY_DOWN && total) { st.globalSel = (st.globalSel + 1) % total; dirty = true; }
            else if (k == KEY_ENTER && total) {
                st.projectRoot = st.projects[st.globalSel].localPath;
                st.screen = SC_PROJECT;
                st.statusMsg = "Project: " + st.projects[st.globalSel].name;
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'a') {
                StartForm(st, FORM_ADD_PROJECT, -1);
                st.statusMsg = "Editing " + st.form.labels[0];
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'e') {
                if (total) {
                    StartForm(st, FORM_EDIT_PROJECT, st.globalSel);
                    st.statusMsg = "Editing " + st.form.labels[0];
                    dirty = true;
                }
            } else if (k == KEY_CHAR && g_lastKeyChar == 'd') {
                if (total) {
                    std::string nm = st.projects[st.globalSel].name;
                    st.projects.erase(st.projects.begin() + st.globalSel);
                    SaveProjects(st.projects);
                    if (st.globalSel >= (int)st.projects.size() && st.globalSel > 0) st.globalSel--;
                    st.statusMsg = "\x1b[1;33m[Deleted]\x1b[0m " + nm + " (folder kept).";
                    dirty = true;
                }
            } else if (k == KEY_CHAR && g_lastKeyChar == 'c') {
                st.screen = SC_PROFILES;
                st.statusMsg = "Connection profiles.";
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'r') {
                Reload(st);
                st.statusMsg = "Refreshed.";
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 's') {
                if (total) {
                    const Project proj = st.projects[st.globalSel];
                    st.statusMsg = TuiStatus(st, proj);
                } else {
                    st.statusMsg = "No projects.";
                }
                dirty = true;
            } else if (k == KEY_CHAR && (g_lastKeyChar == 'p' || g_lastKeyChar == 'l')) {
                if (total) {
                    const Project proj = st.projects[st.globalSel];
                    st.statusMsg = TuiSync(st, proj, g_lastKeyChar == 'p');
                    Reload(st);
                    dirty = true;
                }
            } else if (k == KEY_ESC) {
                running = false;
            }
        }
        // ---- Profiles / settings ----
        else if (st.screen == SC_PROFILES) {
            int total = (int)st.profiles.size();
            if (k == KEY_UP && total) { st.profileSel = (st.profileSel - 1 + total) % total; dirty = true; }
            else if (k == KEY_DOWN && total) { st.profileSel = (st.profileSel + 1) % total; dirty = true; }
            else if (k == KEY_CHAR && g_lastKeyChar == 'a') {
                StartForm(st, FORM_ADD_PROFILE, -1);
                st.statusMsg = "Editing " + st.form.labels[0];
                dirty = true;
            } else if ((k == KEY_ENTER || (k == KEY_CHAR && g_lastKeyChar == 'e')) && total) {
                StartForm(st, FORM_EDIT_PROFILE, st.profileSel);
                st.statusMsg = "Editing " + st.form.labels[0];
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'd') {
                if (total) {
                    std::string nm = st.profiles[st.profileSel].name;
                    KeyringDelete(nm);
                    st.profiles.erase(st.profiles.begin() + st.profileSel);
                    SaveProfiles(st.profiles);
                    if (st.profileSel >= (int)st.profiles.size() && st.profileSel > 0) st.profileSel--;
                    st.statusMsg = "\x1b[1;33m[Deleted]\x1b[0m profile " + nm + ".";
                    dirty = true;
                }
            } else if (k == KEY_ESC) {
                st.screen = SC_GLOBAL;
                st.statusMsg = "Ready.";
                dirty = true;
            }
        }
        // ---- Project ----
        else if (st.screen == SC_PROJECT) {
            if (k == KEY_CHAR && g_lastKeyChar == 'g') {
                st.screen = SC_GLOBAL;
                st.statusMsg = "Global.";
                Reload(st);
                dirty = true;
            } else if (k == KEY_CHAR && (g_lastKeyChar == 'p' || g_lastKeyChar == 'l')) {
                const Project* cur = nullptr;
                for (const auto& p : st.projects)
                    if (p.localPath == st.projectRoot) { cur = &p; break; }
                if (cur) {
                    st.statusMsg = TuiSync(st, *cur, g_lastKeyChar == 'p');
                    Reload(st);
                } else {
                    st.statusMsg = "This folder is not registered yet - press [c] then add a project.";
                }
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'c') {
                st.screen = SC_PROFILES;
                st.statusMsg = "Connection profiles.";
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 'e') {
                int idx = -1;
                for (size_t i = 0; i < st.projects.size(); ++i)
                    if (st.projects[i].localPath == st.projectRoot) { idx = (int)i; break; }
                if (idx >= 0) {
                    StartForm(st, FORM_EDIT_PROJECT, idx);
                    st.form.returnScreen = SC_PROJECT;
                    st.statusMsg = "Editing " + st.form.labels[0];
                } else {
                    st.statusMsg = "This folder is not registered yet - press [c] then add a project.";
                }
                dirty = true;
            } else if (k == KEY_CHAR && g_lastKeyChar == 's') {
                const Project* cur = nullptr;
                for (const auto& p : st.projects)
                    if (p.localPath == st.projectRoot) { cur = &p; break; }
                if (cur) st.statusMsg = TuiStatus(st, *cur);
                else st.statusMsg = "This folder is not registered yet.";
                Reload(st);
                dirty = true;
            } else if (k == KEY_ESC) {
                running = false;
            }
        }
    }

    Out("\x1b[?25h");  // show cursor
    Out("\x1b[2J\x1b[H");
    return 0;
}

// ===========================================================================
// Help
// ===========================================================================
static void PrintHelp() {
    Out(
        "nxzilla - terminal bridge to your remote server\n"
        "\n"
        "Usage:\n"
        "  nxzilla                    inside a project: sync; outside: global space\n"
        "  nxzilla projects           open global space (project list)\n"
        "  nxzilla open <name|path>   open/sync a registered project\n"
        "  nxzilla add                register the current folder as a project\n"
        "  nxzilla remove <name>      remove from registry (folder is kept)\n"
        "  nxzilla scan [folder]      find folders with .nxzilla.json and register\n"
        "  nxzilla pull               pull changes from the server\n"
        "  nxzilla push               push changes to the server\n"
        "  nxzilla --force push       override conflicts (overwrite safely)\n"
        "  nxzilla push --dry-run     show what would transfer, change nothing\n"
        "  nxzilla push --checksum    compare by SHA-1 hash (accurate, slower)\n"
        "  nxzilla status             sync status (read-only)\n"
        "  nxzilla ls [path]          list a remote folder\n"
        "  nxzilla use <profile>      set the profile for this working folder\n"
        "  nxzilla env [name]         list/switch environment (staging/prod)\n"
        "  nxzilla edit <file>        pull a file, edit it, upload it back\n"
        "  nxzilla diff <file>        show differences (remote vs local)\n"
        "  nxzilla watch [secs]       poll for changes (--once to run a single pass)\n"
        "  nxzilla ignore <pattern>   add an ignore pattern\n"
        "  nxzilla --help             help\n"
        "  nxzilla --version          version\n"
        "\n"
        "Running `nxzilla` in a folder without .nxzilla.json opens the global\n"
        "space (a list of every project using Nxzilla).\n");
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    PlatformInit();

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) if (argv[i]) args.push_back(argv[i]);

    bool force = false;
    for (const auto& x : args) if (x == "--force" || x == "-f") force = true;
    bool dryRun = false;
    for (const auto& x : args) if (x == "--dry-run" || x == "-n") dryRun = true;
    bool checksum = false;
    for (const auto& x : args) if (x == "--checksum" || x == "-c") checksum = true;
    bool refresh = false;
    for (const auto& x : args) if (x == "--refresh") refresh = true;

    if (!args.empty()) {
        const std::string& a = args[0];
        if (a == "--version" || a == "-V") {
            Out(std::string("nxzilla ") + NXZILLA_VERSION + "\n");
            return 0;
        }
        if (a == "--help" || a == "-h") {
            PrintHelp();
            return 0;
        }
        if (a == "add" || a == "add-project") {
            if (!IsTty()) {
                Out("nxzilla: `add` needs an interactive terminal.\n");
                return 1;
            }
            AppState st;
            st.screen = SC_GLOBAL;
            StartForm(st, FORM_ADD_PROJECT, -1);
            st.statusMsg = "Editing " + st.form.labels[0];
            return RunTui(st, false);
        }
        if (a == "ls" || a == "pull" || a == "push" || a == "status") {
            // Tentukan proyek: dari cwd (registry) atau argumen nama proyek.
            std::string cwd0 = CurrentDir();
            std::vector<Project> ps = LoadProjects();
            std::vector<Profile> prs = LoadProfiles();
            const Project* proj = nullptr;
            bool hasName = (args.size() >= 2 && args[1][0] != '-');
            if (hasName) {
                for (const auto& p : ps) if (p.name == args[1]) { proj = &p; break; }
                if (!proj) { Out("nxzilla: project '" + args[1] + "' not found.\n"); return 1; }
            } else {
                for (const auto& p : ps) if (p.localPath == cwd0) { proj = &p; break; }
                if (!proj) {
                    Out("nxzilla: this folder is not a registered project.\n"
                        "Run `nxzilla add` here and pick a profile first.\n");
                    return 1;
                }
            }
            // Bila folder proyek punya .nxzilla.json dengan environment aktif,
            // pakai profil/remotePath efektifnya (menimpa nilai registry).
            std::string wantProfile = proj->profile;
            std::string wantRemote = proj->remotePath;
            {
                std::string cfgProf, cfgRemote;
                if (LoadProjectConfig(proj->localPath, cfgProf, cfgRemote) && !cfgProf.empty()) {
                    wantProfile = cfgProf;
                    wantRemote = cfgRemote;
                }
            }
            const Profile* pr = nullptr;
            for (const auto& q : prs) if (q.name == wantProfile) { pr = &q; break; }
            if (!pr) {
                Out("nxzilla: profile '" + wantProfile + "' not found in connections.json.\n");
                return 1;
            }
            if (pr->host.empty()) {
                Out("nxzilla: profile '" + pr->name + "' has no host configured.\n"
                    "Run `nxzilla` -> press [c] to edit the profile.\n");
                return 1;
            }
            std::string effectivePath = wantRemote.empty() ? pr->remotePath : wantRemote;
            Profile use = *pr;
            use.remotePath = effectivePath;

            // Registry basi: folder proyek hilang/dipindah.
            if (!PathExists(proj->localPath)) {
                if (a == "pull") {
                    Out("nxzilla: local folder is missing; creating it:\n  " + proj->localPath + "\n");
                    EnsureDir(proj->localPath);
                } else if (a != "ls") {
                    Out("nxzilla: local folder is missing (stale entry):\n  " + proj->localPath + "\n"
                        "Use `nxzilla pull " + proj->name + "` to recreate it.\n");
                    return 1;
                }
            }

            if (a == "ls") {
                std::vector<std::string> names;
                std::string err;
                if (!RemoteList(use, effectivePath, names, err)) {
                    Out("nxzilla: cannot list " + pr->host + ":" + std::to_string(pr->port) +
                        effectivePath + "\n" + err + "\n");
                    return 1;
                }
                Out("Remote " + pr->protocol + "://" + pr->host + ":" +
                    std::to_string(pr->port) + effectivePath + "  (" +
                    std::to_string(names.size()) + " entries)\n");
                for (const auto& n : names) Out("  " + n + "\n");
                return 0;
            }
            if (a == "status") {
                std::vector<std::string> ignore = LoadProjectIgnore(proj->localPath);
                std::vector<DiffItem> plan;
                std::string err;
                int ttl = (checksum || refresh) ? 0 : 10;  // status: cache 10s
                if (!BuildPlan(use, effectivePath, proj->localPath, ignore, plan, err, checksum, ttl)) {
                    Out("nxzilla: cannot scan: " + err + "\n");
                    return 1;
                }
                Out("Project : " + proj->name + "\n");
                Out("Local   : " + proj->localPath + "\n");
                Out("Profile : " + pr->name + " (" + pr->protocol + " " + pr->user + "@" +
                    pr->host + ":" + std::to_string(pr->port) + ")\n");
                Out("Remote  : " + effectivePath + "\n");
                Out("\n");
                if (plan.empty()) {
                    Out(std::string("\x1b[1;32m") + "[synced]" + kReset + " local and remote match.\n");
                } else {
                    int aN = 0, dN = 0, mN = 0, cN = 0;
                    for (const auto& it : plan) {
                        if (it.kind == 'A') { aN++; Out(std::string("  \x1b[1;33mA\x1b[0m ") + it.rel + "  (local only)\n"); }
                        else if (it.kind == 'D') { dN++; Out(std::string("  \x1b[1;36mD\x1b[0m ") + it.rel + "  (remote only)\n"); }
                        else {
                            mN++;
                            if (it.conflict) cN++;
                            Out(std::string("  ") + (it.conflict ? "\x1b[1;31mC" : "\x1b[1;33mM") + "\x1b[0m "
                                + it.rel + "  (" + std::to_string(it.localSize) + " vs " +
                                std::to_string(it.remoteSize) + " bytes, " +
                                (it.localNewer ? "local newer" : "remote newer") +
                                (it.conflict ? ", CONFLICT" : "") + ")\n");
                        }
                    }
                    Out("\n" + std::to_string(plan.size()) + " change(s): " +
                        std::to_string(aN) + " local-only, " + std::to_string(dN) +
                        " remote-only, " + std::to_string(mN) + " modified" +
                        (cN ? (", " + std::to_string(cN) + " CONFLICT") : "") + ".\n");
                    if (cN) Out(std::string(kGray) +
                        "  Conflicts: both sides changed since last sync. Use --force to override.\n" + kReset);
                }
                return 0;
            }
            std::vector<std::string> ignore = LoadProjectIgnore(proj->localPath);
            std::vector<DiffItem> plan;
            std::string err;
            if (!BuildPlan(use, effectivePath, proj->localPath, ignore, plan, err, checksum)) {
                Out("nxzilla: cannot scan remote: " + err + "\n");
                return 1;
            }
            auto tStart = std::chrono::steady_clock::now();
            if (a == "push") {
                int done = 0, conflicts = 0;
                long long bytes = 0;
                if (dryRun) {
                    Out("[dry-run] " + std::to_string(plan.size()) + " change(s) vs " +
                        pr->protocol + "://" + pr->host + ":" + std::to_string(pr->port) +
                        effectivePath + "\n");
                    for (const auto& it : plan) {
                        if (it.kind == 'D') Out(std::string("  \x1b[90mD\x1b[0m ") + it.rel + "  (skip: remote only)\n");
                        else if (it.conflict) Out(std::string("  \x1b[1;31mC\x1b[0m ") + it.rel + "  (conflict: would skip without --force)\n");
                        else Out(std::string("  \x1b[1;32m^\x1b[0m ") + it.rel + "  (would push)\n");
                    }
                    Out("[dry-run] no changes made.\n");
                    return 0;
                }
                std::vector<std::pair<std::string, std::string>> batch;  // (localFile, rel)
                for (const auto& it : plan) {
                    if (it.kind == 'D') continue;  // hanya remote, jangan kirim
                    if (it.conflict && !force) {
                        Out("  \x1b[1;31mC\x1b[0m " + it.rel + "  (conflict, skipped; use --force)\n");
                        conflicts++;
                        continue;
                    }
                    std::string localFile = proj->localPath;
                    if (localFile.back() != '/' && localFile.back() != '\\') localFile += kPathSep;
                    std::string rel = it.rel;
                    if (!SafeRel(rel)) { Out("  \x1b[1;31m!\x1b[0m skipping unsafe path: " + rel + "\n"); continue; }
                    for (char& c : rel) if (c == '/') c = kPathSep;
                    localFile += rel;
                    size_t slash = it.rel.find_last_of('/');
                    if (slash != std::string::npos) {
                        std::string dirRel = it.rel.substr(0, slash);
                        std::string derr;
                        RemoteMkdir(use, dirRel, derr);
                    }
                    batch.push_back({localFile, it.rel});
                }
                if (!IsSsh(use) && !batch.empty()) {
                    std::string berr;
                    if (FtpBatchPut(use, batch, berr)) {
                        // Ambil mtime semua berkas dalam satu HEAD batch.
                        std::vector<std::string> rels;
                        for (const auto& b : batch) rels.push_back(b.second);
                        std::vector<RemoteEntry> metas;
                        FtpHeadMany(use, rels, metas);
                        for (size_t i = 0; i < batch.size(); ++i) {
                            Out(std::string("  \x1b[1;32m^\x1b[0m ") + batch[i].second + "\n");
                            if (i < metas.size() && metas[i].mtime > 0)
                                SetFileMTime(batch[i].first, metas[i].mtime);
                            bytes += FileSize(batch[i].first);
                            done++;
                        }
                    } else {
                        Out("nxzilla: batch push failed: " + berr + "\n");
                        return 1;
                    }
                } else {
                    for (const auto& b : batch) {
                        std::string perr;
                        if (!RemotePut(use, b.second, b.first, perr)) {
                            Out("  \x1b[1;31m!\x1b[0m " + b.second + "  (" + perr + ")\n");
                            continue;
                        }
                        long long rs = 0, rm = 0;
                        if (RemoteHeadMeta(use, b.second, rs, rm) && rm > 0) SetFileMTime(b.first, rm);
                        bytes += FileSize(b.first);
                        Out(std::string("  \x1b[1;32m^\x1b[0m ") + b.second + "\n");
                        done++;
                    }
                }
                for (auto& p : ps) if (p.name == proj->name) {
                    p.lastStatus = done ? "synced" : p.lastStatus;
                    char tb[64]; time_t t = time(nullptr); tm* lt = localtime(&t);
                    if (lt) strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", lt); else tb[0] = 0;
                    p.lastSync = tb;
                }
                SaveProjects(ps);
                SaveSyncedBaseline(use, effectivePath, proj->localPath);
                double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - tStart).count();
                Out(TransferSummary("Pushed", done, bytes, elapsed) + ".\n");
                if (conflicts) Out(std::to_string(conflicts) + " conflict(s) skipped (use --force).\n");
                return 0;
            }
            // pull
            int done = 0, conflicts = 0;
            long long bytes = 0;
            if (dryRun) {
                Out("[dry-run] " + std::to_string(plan.size()) + " change(s) from " +
                    pr->protocol + "://" + pr->host + ":" + std::to_string(pr->port) +
                    effectivePath + "\n");
                for (const auto& it : plan) {
                    if (it.kind == 'A') Out(std::string("  \x1b[90mA\x1b[0m ") + it.rel + "  (skip: local only)\n");
                    else if (it.conflict) Out(std::string("  \x1b[1;31mC\x1b[0m ") + it.rel + "  (conflict: would skip without --force)\n");
                    else Out(std::string("  \x1b[1;32mv\x1b[0m ") + it.rel + "  (would pull)\n");
                }
                Out("[dry-run] no changes made.\n");
                return 0;
            }
            // Kumpulkan berkas yang akan ditransfer; pakai batch (satu koneksi)
            // bila transportnya FTP.
            std::vector<std::pair<std::string, std::string>> batch;  // (rel, localFile)
            for (const auto& it : plan) {
                if (it.kind == 'A') continue;  // hanya lokal, jangan timpa
                if (it.conflict && !force) {
                    Out("  \x1b[1;31mC\x1b[0m " + it.rel + "  (conflict, skipped; use --force)\n");
                    conflicts++;
                    continue;
                }
                std::string localFile = proj->localPath;
                if (localFile.back() != '/' && localFile.back() != '\\') localFile += kPathSep;
                std::string rel = it.rel;
                if (!SafeRel(rel)) { Out("  \x1b[1;31m!\x1b[0m skipping unsafe path: " + rel + "\n"); continue; }
                for (char& c : rel) if (c == '/') c = kPathSep;
                localFile += rel;
                size_t slash = localFile.find_last_of("/\\");
                if (slash != std::string::npos) EnsureDir(localFile.substr(0, slash));
                batch.push_back({it.rel, localFile});
            }
            if (!IsSsh(use) && !batch.empty()) {
                std::string berr;
                if (FtpBatchGet(use, batch, berr)) {
                    std::vector<std::string> rels;
                    for (const auto& b : batch) rels.push_back(b.first);
                    std::vector<RemoteEntry> metas;
                    FtpHeadMany(use, rels, metas);
                    for (size_t i = 0; i < batch.size(); ++i) {
                        Out(std::string("  \x1b[1;32mv\x1b[0m ") + batch[i].first + "\n");
                        if (i < metas.size() && metas[i].mtime > 0)
                            SetFileMTime(batch[i].second, metas[i].mtime);
                        long long rm = (i < metas.size()) ? metas[i].mtime : 0;
                        if (rm > 0) bytes += FileSize(batch[i].second);
                        done++;
                    }
                } else {
                    Out("nxzilla: batch pull failed: " + berr + "\n");
                    return 1;
                }
            } else {
                for (const auto& b : batch) {
                    std::string gerr;
                    if (!RemoteGet(use, b.first, b.second, gerr)) {
                        Out("  \x1b[1;31m!\x1b[0m " + b.first + "  (" + gerr + ")\n");
                        continue;
                    }
                    long long rs = 0, rm = 0;
                    if (RemoteHeadMeta(use, b.first, rs, rm) && rm > 0) SetFileMTime(b.second, rm);
                    long long sz = FileSize(b.second);
                    if (sz > 0) bytes += sz;
                    Out(std::string("  \x1b[1;32mv\x1b[0m ") + b.first + "\n");
                    done++;
                }
            }
            for (auto& p : ps) if (p.name == proj->name) {
                p.lastStatus = done ? "synced" : p.lastStatus;
                char tb[64]; time_t t = time(nullptr); tm* lt = localtime(&t);
                if (lt) strftime(tb, sizeof(tb), "%Y-%m-%dT%H:%M:%SZ", lt); else tb[0] = 0;
                p.lastSync = tb;
            }
            SaveProjects(ps);
            SaveSyncedBaseline(use, effectivePath, proj->localPath);
            double elapsed2 = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - tStart).count();
            Out(TransferSummary("Pulled", done, bytes, elapsed2) + ".\n");
            if (conflicts) Out(std::to_string(conflicts) + " conflict(s) skipped (use --force).\n");
            return 0;
        }
        if (a == "env") {
            std::string cwd0 = CurrentDir();
            std::vector<EnvEntry> envs;
            std::string prof, rp, active;
            if (!LoadProjectFull(cwd0, prof, rp, envs, active)) {
                Out("nxzilla: no .nxzilla.json here (run inside a project).\n");
                return 1;
            }
            // `env` tanpa argumen: daftar.
            if (args.size() < 2 || args[1][0] == '-') {
                if (envs.empty()) {
                    Out("nxzilla: no environments defined. (using profile '" + prof +
                        "', remote '" + rp + "')\n");
                } else {
                    Out("Environments" + (active.empty() ? "" : " (active: " + active + ")") + ":\n");
                    for (const auto& e : envs) {
                        std::string mark = (e.name == active) ? " *" : "  ";
                        Out(mark + " " + e.name + "  (profile: " + e.profile +
                            ", remote: " + e.remotePath + ")\n");
                    }
                }
                return 0;
            }
            // `env <nama>`: set aktif.
            const std::string& want = args[1];
            bool found = false;
            for (const auto& e : envs) if (e.name == want) { found = true; break; }
            if (!found) {
                Out("nxzilla: environment '" + want + "' not found.\n");
                if (!envs.empty()) { Out("Available: "); for (const auto& e : envs) Out(e.name + " "); Out("\n"); }
                return 1;
            }
            std::vector<std::string> ig = LoadProjectIgnore(cwd0);
            std::string curProf, curRp;
            LoadProjectConfig(cwd0, curProf, curRp);
            // Simpan: SaveProjectConfig mempertahankan environments; kita perlu set 'active'.
            // Tulis ulang lewat helper khusus.
            SaveEnvironments(cwd0, envs, want, ig);
            Out("nxzilla: active environment set to '" + want + "'.\n");
            return 0;
        }
        if (a == "use" && args.size() >= 2) {
            std::string cwd0 = CurrentDir();
            std::vector<Profile> prs = LoadProfiles();
            bool found = false;
            for (const auto& p : prs) if (p.name == args[1]) { found = true; break; }
            if (!found) { Out("nxzilla: profile '" + args[1] + "' not found.\n"); return 1; }
            std::string prof, rp;
            LoadProjectConfig(cwd0, prof, rp);
            std::vector<std::string> ig = LoadProjectIgnore(cwd0);
            if (!SaveProjectConfig(cwd0, args[1], rp, ig)) {
                Out("nxzilla: can't write .nxzilla.json here.\n");
                return 1;
            }
            // Perbarui registry bila folder ini terdaftar.
            std::vector<Project> reg = LoadProjects();
            bool touched = false;
            for (auto& p : reg) if (p.localPath == cwd0) { p.profile = args[1]; touched = true; }
            if (touched) SaveProjects(reg);
            Out("nxzilla: profile set to '" + args[1] + "' for " + cwd0 + ".\n");
            return 0;
        }
        if (a == "ignore") {
            std::string cwd0 = CurrentDir();
            std::vector<std::string> ig = LoadProjectIgnore(cwd0);
            if (args.size() < 2 || args[1][0] == '-') {
                if (ig.empty()) Out("nxzilla: no ignore patterns.\n");
                else { Out("Ignore patterns:\n"); for (const auto& p : ig) Out("  " + p + "\n"); }
                return 0;
            }
            const std::string& pat = args[1];
            bool exists = false;
            for (const auto& p : ig) if (p == pat) { exists = true; break; }
            if (!exists) ig.push_back(pat);
            std::string prof, rp;
            LoadProjectConfig(cwd0, prof, rp);
            if (!SaveProjectConfig(cwd0, prof, rp, ig)) {
                Out("nxzilla: can't write .nxzilla.json here.\n");
                return 1;
            }
            Out(exists ? "nxzilla: pattern already present: " : "nxzilla: ignore added: ");
            Out(pat + "\n");
            return 0;
        }
        if (a == "watch") {
            std::string cwd0 = CurrentDir();
            std::vector<Project> ps = LoadProjects();
            std::vector<Profile> prs = LoadProfiles();
            const Project* proj = nullptr;
            bool hasName = (args.size() >= 2 && args[1][0] != '-' &&
                            !isdigit((unsigned char)args[1][0]));
            if (hasName) {
                for (const auto& p : ps) if (p.name == args[1]) { proj = &p; break; }
                if (!proj) { Out("nxzilla: project '" + args[1] + "' not found.\n"); return 1; }
            } else {
                for (const auto& p : ps) if (p.localPath == cwd0) { proj = &p; break; }
                if (!proj) { Out("nxzilla: this folder is not a registered project.\n"); return 1; }
            }
            const Profile* pr = nullptr;
            for (const auto& q : prs) if (q.name == proj->profile) { pr = &q; break; }
            if (!pr) { Out("nxzilla: profile not found.\n"); return 1; }
            if (pr->host.empty()) { Out("nxzilla: profile '" + pr->name + "' has no host.\n"); return 1; }
            std::string effPath = proj->remotePath.empty() ? pr->remotePath : proj->remotePath;
            Profile use = *pr;
            use.remotePath = effPath;

            // Interval detik (default 10). --once = sekali lalu keluar.
            int interval = 10;
            bool once = false;
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--once") once = true;
                else if (!args[i].empty() && isdigit((unsigned char)args[i][0])) interval = atoi(args[i].c_str());
            }
            if (interval < 1) interval = 1;

            std::vector<std::string> ignore = LoadProjectIgnore(proj->localPath);
            Out("Watching " + proj->name + " (" + pr->protocol + "://" + pr->host + ":" +
                std::to_string(pr->port) + effPath + ") every " + std::to_string(interval) + "s");
            Out(once ? " (once).\n" : ". Press Ctrl+C to stop.\n");

            int iter = 0;
            for (;;) {
                iter++;
                std::vector<DiffItem> plan;
                std::string err;
                char tb[32];
                time_t now = time(nullptr); tm* lt = localtime(&now);
                if (lt) strftime(tb, sizeof(tb), "%H:%M:%S", lt); else tb[0] = 0;
                if (!BuildPlan(use, effPath, proj->localPath, ignore, plan, err)) {
                    Out("[" + std::string(tb) + "] error: " + err + "\n");
                } else if (plan.empty()) {
                    Out("[" + std::string(tb) + "] up to date.\n");
                } else {
                    int aN = 0, dN = 0, mN = 0, cN = 0;
                    for (const auto& it : plan) {
                        if (it.kind == 'A') aN++;
                        else if (it.kind == 'D') dN++;
                        else { mN++; if (it.conflict) cN++; }
                    }
                    Out("[" + std::string(tb) + "] " + std::to_string(plan.size()) +
                        " change(s): " + std::to_string(aN) + " local-only, " +
                        std::to_string(dN) + " remote-only, " + std::to_string(mN) +
                        " modified" + (cN ? (", " + std::to_string(cN) + " CONFLICT") : "") + "\n");
                }
                if (once) return 0;
                SleepMs(interval * 1000);
            }
        }
        if (a == "diff" && args.size() >= 2 && args[1][0] != '-') {
            std::string cwd0 = CurrentDir();
            std::vector<Project> ps = LoadProjects();
            std::vector<Profile> prs = LoadProfiles();
            const Project* proj = nullptr;
            for (const auto& p : ps) if (p.localPath == cwd0) { proj = &p; break; }
            if (!proj) { Out("nxzilla: this folder is not a registered project.\n"); return 1; }
            const Profile* pr = nullptr;
            for (const auto& q : prs) if (q.name == proj->profile) { pr = &q; break; }
            if (!pr || pr->host.empty()) { Out("nxzilla: profile missing or has no host.\n"); return 1; }
            std::string effPath = proj->remotePath.empty() ? pr->remotePath : proj->remotePath;
            Profile use = *pr;
            use.remotePath = effPath;

            std::string rel = args[1];
            for (char& c : rel) if (c == '\\') c = '/';
            while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
            if (!SafeRel(rel)) { Out("nxzilla: unsafe path.\n"); return 1; }

            std::string localFile = proj->localPath;
            if (localFile.back() != '/' && localFile.back() != '\\') localFile += kPathSep;
            std::string relOs = rel;
            for (char& c : relOs) if (c == '/') c = kPathSep;
            localFile += relOs;

            std::string localText = ReadFileText(localFile);
            if (localText.empty() && !FileExists(localFile))
                Out("nxzilla: (local file missing)\n");

            // Ambil versi remote ke file sementara.
            EnsureDir(ConfigDir());
            std::string tmp;
            JoinPath(ConfigDir(), "diff_tmp", tmp);
            std::string gerr;
            if (!RemoteGet(use, rel, tmp, gerr)) {
                Out("nxzilla: cannot fetch remote '" + rel + "': " + gerr + "\n");
                remove(tmp.c_str());
                return 1;
            }
            std::string remoteText = ReadFileText(tmp);
            remove(tmp.c_str());

            int changes = PrintDiff(remoteText, localText, "remote:" + rel, "local:" + rel);
            if (changes == 0) Out("nxzilla: '" + rel + "' is identical (remote vs local).\n");
            else Out("\n" + std::to_string(changes) + " line(s) differ  (- remote, + local).\n");
            return 0;
        }
        if (a == "edit" && args.size() >= 2 && args[1][0] != '-') {
            std::string cwd0 = CurrentDir();
            std::vector<Project> ps = LoadProjects();
            std::vector<Profile> prs = LoadProfiles();
            const Project* proj = nullptr;
            for (const auto& p : ps) if (p.localPath == cwd0) { proj = &p; break; }
            if (!proj) {
                Out("nxzilla: this folder is not a registered project.\n");
                return 1;
            }
            const Profile* pr = nullptr;
            for (const auto& q : prs) if (q.name == proj->profile) { pr = &q; break; }
            if (!pr) { Out("nxzilla: profile not found.\n"); return 1; }
            if (pr->host.empty()) { Out("nxzilla: profile '" + pr->name + "' has no host.\n"); return 1; }
            std::string effPath = proj->remotePath.empty() ? pr->remotePath : proj->remotePath;
            Profile use = *pr;
            use.remotePath = effPath;

            // rel = argumen, dengan pemisah '/'
            std::string rel = args[1];
            for (char& c : rel) if (c == '\\') c = '/';
            while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);

            std::string localFile = proj->localPath;
            if (localFile.back() != '/' && localFile.back() != '\\') localFile += kPathSep;
            std::string relOs = rel;
            for (char& c : relOs) if (c == '/') c = kPathSep;
            localFile += relOs;
            size_t slash = localFile.find_last_of("/\\");
            if (slash != std::string::npos) EnsureDir(localFile.substr(0, slash));

            // Tarik versi terbaru dari server (abaikan bila gagal: berkas baru).
            std::string gerr;
            if (RemoteGet(use, rel, localFile, gerr)) {
                long long rs = 0, rm = 0;
                if (RemoteHeadMeta(use, rel, rs, rm) && rm > 0) SetFileMTime(localFile, rm);
                Out("nxzilla: pulled " + rel + "\n");
            } else {
                Out("nxzilla: '" + rel + "' not on server yet (new file).\n");
            }

            // Pilih editor.
            const char* envEd = getenv("EDITOR");
#ifdef _WIN32
            if (!envEd || !*envEd) envEd = getenv("EDITOR");
            std::string editor = (envEd && *envEd) ? envEd : "notepad";
#else
            std::string editor = (envEd && *envEd) ? envEd : "vi";
#endif
            // Editor dari env bisa berisi beberapa kata (mis. "code -w").
            std::string cmd = editor + " " + ShellQuoteD(localFile);
            Out("nxzilla: opening in '" + editor + "' ...\n");
            int rc = RunInteractive(cmd);
            if (rc != 0) {
                Out("nxzilla: editor exited with code " + std::to_string(rc) + ".\n");
            }

            // Unggah balik.
            std::string perr;
            if (!RemotePut(use, rel, localFile, perr)) {
                Out("nxzilla: upload failed: " + perr + "\n");
                return 1;
            }
            long long rs = 0, rm = 0;
            if (RemoteHeadMeta(use, rel, rs, rm) && rm > 0) SetFileMTime(localFile, rm);
            Out("nxzilla: uploaded " + rel + "\n");
            return 0;
        }
        if (a == "scan") {
            std::string base = (args.size() >= 2 && args[1][0] != '-') ? args[1] : CurrentDir();
            std::vector<Project> found;
            ScanForProjects(base, 0, found);

            std::vector<Project> reg = LoadProjects();
            int added = 0;
            for (const auto& f : found) {
                bool exists = false;
                for (const auto& r : reg)
                    if (r.localPath == f.localPath) { exists = true; break; }
                if (exists) continue;
                reg.push_back(f);
                added++;
                Out("  + " + f.name + "  " + f.localPath + "  (profile: " +
                    (f.profile.empty() ? "-" : f.profile) + ")\n");
            }
            if (added) SaveProjects(reg);
            Out("Scanned " + base + ": " + std::to_string(found.size()) +
                " project(s) found, " + std::to_string(added) + " newly registered.\n");
            return 0;
        }
        if (a == "remove" && args.size() >= 2) {
            std::vector<Project> ps = LoadProjects();
            size_t before = ps.size();
            ps.erase(std::remove_if(ps.begin(), ps.end(),
                                    [&](const Project& p) { return p.name == args[1]; }),
                     ps.end());
            if (ps.size() == before) {
                Out("nxzilla: project '" + args[1] + "' not found.\n");
                return 1;
            }
            SaveProjects(ps);
            Out("nxzilla: project '" + args[1] + "' removed from registry.\n");
            return 0;
        }
        // Perintah tak dikenal (kata diawali huruf, bukan flag) -> error + help.
        if (!a.empty() && a[0] != '-' && a != "projects" && a != "list") {
            Out("nxzilla: unknown command '" + a + "'.\n");
            Out("Run `nxzilla --help` for usage.\n");
            return 1;
        }
    }

    std::string cwd = CurrentDir();
    std::string root;

    // No TTY (pipe/script): concise text output. (Diabaikan bila mode skrip dev.)
    if (!IsTty() && !getenv("NXZILLA_TUI_SCRIPT")) {
        bool inProject = FindProjectRoot(cwd, root);
        Out(std::string("NXZILLA v") + NXZILLA_VERSION + "\n");
        Out("Working dir : " + cwd + "\n");
        Out("Config      : " + ConfigDir() + "\n");
        if (inProject) {
            Out("Context     : PROJECT (" + root + ")\n");
        } else {
            std::vector<Project> ps = LoadProjects();
            Out("Context     : GLOBAL (" + std::to_string(ps.size()) + " registered projects)\n");
            for (const auto& p : ps) Out("  - " + p.name + "  " + p.localPath + "\n");
        }
        return 0;
    }

    AppState st;
    bool inProject = FindProjectRoot(cwd, root);
    bool forceGlobal = (!args.empty() && (args[0] == "projects" || args[0] == "list"));
    if (inProject) {
        st.screen = SC_PROJECT;
        st.projectRoot = root;
    } else if (!forceGlobal) {
        // Onboarding: folder bukan proyek & belum terdaftar -> tawarkan setup.
        bool registered = false;
        for (const auto& p : LoadProjects()) if (p.localPath == cwd) { registered = true; break; }
        bool haveProfiles = !LoadProfiles().empty();
        if (!registered && haveProfiles) {
            st.screen = SC_ONBOARD;
            st.onboardDir = cwd;
            st.projects = LoadProjects();
            st.profiles = LoadProfiles();
            st.statusMsg = "Press [Enter] to set up this folder.";
        } else {
            st.screen = SC_GLOBAL;
        }
    } else {
        st.screen = SC_GLOBAL;
    }
    return RunTui(st, st.screen != SC_ONBOARD);
}
