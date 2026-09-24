// ringer: find unclaimed usernames on Discord, Roblox and a few other sites.
//
// Windows uses WinHTTP, which is built into Windows, so there's nothing to install.
// Linux and macOS use libcurl.
//
// Everything ringer prints is plain ASCII, so it draws the same in every Windows
// console font, the old raster fonts included.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winhttp.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "winhttp.lib")
#  endif
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#else
#  include <curl/curl.h>
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

namespace {

const char* USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0 Safari/537.36";
const std::string DISCORD_API = "https://discord.com/api/v9";

const char* RESULTS_FILE = "available.txt";
const char* UNCHECKED_FILE = "unchecked.txt";  // what a stopped .txt file run didn't get to
const char* CONFIG_FILE = "ringer.cfg";
const char* OLD_CONFIG_FILE = "usercheck.cfg";  // ringer used to be called usercheck

// Colors. setup_console() blanks them if the console can't do escape codes.
// DIM is dark gray rather than the "faint" code, which the classic Windows console ignores.
const char* GREEN = "\033[92m";
const char* RED = "\033[91m";
const char* YELLOW = "\033[93m";
const char* GOLD = "\033[33m";
const char* CYAN = "\033[96m";
const char* DIM = "\033[90m";
const char* BOLD = "\033[1m";
const char* RESET = "\033[0m";
bool g_vt = true;  // the console understands escape codes

const std::string LOWER = "abcdefghijklmnopqrstuvwxyz";
const std::string UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const std::string DIGITS = "0123456789";

// Ctrl+C during a check stops the run and shows the summary. Anywhere else it quits.
std::atomic<bool> g_checking{false};
std::atomic<bool> g_stop{false};

void on_sigint(int) {
    if (!g_checking) std::_Exit(0);
    g_stop = true;
    std::signal(SIGINT, on_sigint);  // Windows resets the handler after each signal
}

// Sleeps, but wakes up early if Ctrl+C is pressed. `tick` runs every 100ms while waiting.
void nap(double seconds, const std::function<void()>& tick = {}) {
    using namespace std::chrono;
    auto end = steady_clock::now() + duration_cast<steady_clock::duration>(duration<double>(seconds));
    while (!g_stop && steady_clock::now() < end) {
        if (tick) tick();
        std::this_thread::sleep_for(std::min<steady_clock::duration>(milliseconds(100), end - steady_clock::now()));
    }
}

void setup_console() {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode) || !SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        // Older than Windows 10, or not a console at all: escape codes would print as junk.
        g_vt = false;
        GREEN = RED = YELLOW = GOLD = CYAN = DIM = BOLD = RESET = "";
    }
#endif
}

int console_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return info.srWindow.Right - info.srWindow.Left + 1;
    }
#else
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
    return 80;
}

void clear_screen() {
    if (g_vt) {
        // 2J only clears the window. Windows Terminal and newer Windows consoles push what was on
        // it into the scrollback, so every old screen piles up above the new one. 3J clears the
        // scrollback too, like cls does.
        std::cout << "\033[H\033[2J\033[3J" << std::flush;
        return;
    }
    std::cout << std::flush;
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(out, &info)) return;
    DWORD cells = static_cast<DWORD>(info.dwSize.X) * static_cast<DWORD>(info.dwSize.Y), written = 0;
    COORD home = {0, 0};
    FillConsoleOutputCharacterA(out, ' ', cells, home, &written);
    FillConsoleOutputAttribute(out, info.wAttributes, cells, home, &written);
    SetConsoleCursorPosition(out, home);
#endif
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool only_chars(const std::string& s, const std::string& allowed) {
    return !s.empty() && s.find_first_not_of(allowed) == std::string::npos;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool is_unreserved(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (is_unreserved(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Server messages can contain anything. This keeps them on one line and in plain
// ASCII, with each non-ASCII character shown as '?'.
std::string plain_text(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;  // rest of a UTF-8 character
        char ch = c >= 0x80 ? '?' : (c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c);
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ---------------------------------------------------------------------------
// Minimal JSON: the responses we read are tiny and fixed-shape, so a key lookup
// is all we need.
// ---------------------------------------------------------------------------

std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out + "\"";
}

void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool parse_hex4(const std::string& s, size_t pos, unsigned& out) {
    if (pos + 4 > s.size()) return false;
    out = 0;
    for (size_t i = pos; i < pos + 4; ++i) {
        char c = s[i];
        out <<= 4;
        if (c >= '0' && c <= '9') out |= c - '0';
        else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
        else return false;
    }
    return true;
}

// Reads the string that starts with the quote at doc[i] into `out`, unescaped.
bool json_string(const std::string& doc, size_t i, std::string& out) {
    out.clear();
    if (i >= doc.size() || doc[i] != '"') return false;
    for (++i; i < doc.size(); ++i) {
        char c = doc[i];
        if (c == '"') return true;
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= doc.size()) return false;
        switch (doc[i]) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = 0;
                if (!parse_hex4(doc, i + 1, cp)) return false;
                i += 4;
                unsigned lo = 0;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < doc.size() && doc[i + 1] == '\\' &&
                    doc[i + 2] == 'u' && parse_hex4(doc, i + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
                append_utf8(out, cp);
                break;
            }
            default: out += doc[i];  // \" \\ \/
        }
    }
    return false;
}

// Finds the first "key": at or after `from` and puts its value in `out`.
// Strings come back unescaped, numbers/bools/null as their raw text.
bool json_get(const std::string& doc, const std::string& key, std::string& out, size_t from = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t i = doc.find(needle, from);
    if (i == std::string::npos) return false;
    i += needle.size();
    auto skip_ws = [&] {
        while (i < doc.size() && (doc[i] == ' ' || doc[i] == '\t' || doc[i] == '\r' || doc[i] == '\n')) ++i;
    };
    skip_ws();
    if (i >= doc.size() || doc[i] != ':') return false;
    ++i;
    skip_ws();
    if (i >= doc.size()) return false;
    if (doc[i] == '"') return json_string(doc, i, out);
    size_t end = doc.find_first_of(",}] \t\r\n", i);
    out = doc.substr(i, end == std::string::npos ? std::string::npos : end - i);
    return true;
}

// Adds every string value of "key" in `doc` to `into`, lowercased. That's how the names get
// pulled out of a batch lookup's answer.
void json_names(const std::string& doc, const std::string& key, std::set<std::string>& into) {
    const std::string needle = "\"" + key + "\"";
    std::string value;
    for (size_t i = doc.find(needle); i != std::string::npos; i = doc.find(needle, i + 1)) {
        if (json_get(doc, key, value, i) && value != "null") into.insert(to_lower(value));
    }
}

// ["a","b"]
std::string json_list(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : ",") + json_quote(item);
    return "[" + out + "]";
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

struct Response {
    long status = 0;                             // 0 means no answer at all, see `error`
    std::string body;
    std::map<std::string, std::string> headers;  // names lowercased
    std::string error;

    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? "" : it->second;
    }
};

// Extras for the requests that need them.
struct Options {
    std::vector<std::string> headers;               // extra "Name: value" lines
    std::string content_type = "application/json";  // of the body, if there is one
    bool follow_redirects = true;
};

// Adds a "Name: value" header line to `headers`. Anything else, like the status line, is ignored.
void add_header(std::map<std::string, std::string>& headers, const std::string& line) {
    size_t colon = line.find(':');
    if (colon != std::string::npos) headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
}

// Tests build with -DTEST_SERVER='"http://127.0.0.1:8000"' to send every request to a fake server
// instead, with the real host moved into the path: https://discord.com/api/v9/... becomes
// http://127.0.0.1:8000/discord.com/api/v9/...
std::string route(const std::string& url) {
#ifdef TEST_SERVER
    return TEST_SERVER "/" + url.substr(url.find("://") + 3);
#else
    return url;
#endif
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

struct WinHttpHandle {
    HINTERNET h;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

Response http(const std::string& method, const std::string& url, const std::string& body = "",
              const Options& opt = {}) {
    Response r;
    static HINTERNET session = [] {
        HINTERNET s = WinHttpOpen(widen(USER_AGENT).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (s) WinHttpSetTimeouts(s, 15000, 15000, 15000, 15000);
        return s;
    }();
    if (!session) {
        r.error = "couldn't start WinHTTP";
        return r;
    }

    std::wstring wurl = widen(route(url));
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        r.error = "bad URL";
        return r;
    }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    WinHttpHandle conn(WinHttpConnect(session, host.c_str(), uc.nPort, 0));
    if (!conn.h) {
        r.error = "couldn't connect (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle req(WinHttpOpenRequest(conn.h, widen(method).c_str(), path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req.h) {
        r.error = "couldn't open request (error " + std::to_string(GetLastError()) + ")";
        return r;
    }

    if (!opt.follow_redirects) {
        DWORD off = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
    }

    std::string head = "Accept: application/json, text/html\r\n";
    if (!body.empty()) head += "Content-Type: " + opt.content_type + "\r\n";
    for (const std::string& h : opt.headers) head += h + "\r\n";
    const std::wstring headers = widen(head);
    DWORD len = static_cast<DWORD>(body.size());
    LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    if (!WinHttpSendRequest(req.h, headers.c_str(), static_cast<DWORD>(-1), data, len, len, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        DWORD err = GetLastError();
        r.error = err == ERROR_WINHTTP_TIMEOUT ? "timed out" : "request failed (error " + std::to_string(err) + ")";
        return r;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<long>(status);

    // All the headers at once, one per line. The first call just says how much room they need.
    size = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    std::wstring raw(size / sizeof(wchar_t) + 1, L'\0');
    if (size && WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0],
                                    &size, WINHTTP_NO_HEADER_INDEX)) {
        std::string line;
        for (DWORD i = 0; i < size / sizeof(wchar_t); ++i) {
            if (raw[i] == L'\n') {
                add_header(r.headers, line);
                line.clear();
            } else if (raw[i] != L'\r') {
                line += raw[i] < 0x80 ? static_cast<char>(raw[i]) : '?';
            }
        }
        add_header(r.headers, line);
    }

    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        if (!WinHttpReadData(req.h, &chunk[0], avail, &got) || got == 0) break;
        r.body.append(chunk.data(), got);
    }
    return r;
}

#else

size_t on_body(char* p, size_t size, size_t n, void* userdata) {
    static_cast<std::string*>(userdata)->append(p, size * n);
    return size * n;
}

size_t on_header(char* p, size_t size, size_t n, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(p, size * n);
    if (starts_with(to_lower(line), "http/")) headers->clear();  // new response after a redirect
    else add_header(*headers, line);
    return size * n;
}

Response http(const std::string& method, const std::string& url, const std::string& body = "",
              const Options& opt = {}) {
    Response r;
    static CURL* curl = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return curl_easy_init();
    }();
    if (!curl) {
        r.error = "couldn't start libcurl";
        return r;
    }

    const std::string where = route(url);
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, where.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, opt.follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r.headers);

    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json, text/html");
    for (const std::string& h : opt.headers) headers = curl_slist_append(headers, h.c_str());
    if (method == "POST") {
        headers = curl_slist_append(headers, ("Content-Type: " + opt.content_type).c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) {
        r.error = curl_easy_strerror(rc);
        return r;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
}

#endif

// How long a 429 says to wait: Discord puts it in the body, most sites in the header.
// 0 means the server didn't say.
double retry_after_seconds(const Response& r) {
    std::string value;
    double seconds = 0;
    try {
        if (json_get(r.body, "retry_after", value)) seconds = std::stod(value);
        else if (!r.header("retry-after").empty()) seconds = std::stod(r.header("retry-after"));
    } catch (const std::exception&) {
    }
    return seconds > 0 ? std::max(seconds, 1.0) : 0;
}

// ---------------------------------------------------------------------------
// Platforms
// ---------------------------------------------------------------------------

enum class State { Available, Taken, Invalid, Error, RateLimited };

struct Result {
    State state;
    std::string detail;
    double retry_after = 0;    // for RateLimited: seconds the site asked for, 0 if it didn't say
    bool unconfirmed = false;  // for Available: only the looser check saw it (see check_discord)
};

// Looks up a batch of names in one request. Returns false with `why` set (an Error or RateLimited
// result) if it couldn't, otherwise adds the names that belong to an account to `found`, lowercased.
using Lookup = std::function<bool(const std::vector<std::string>& names, std::set<std::string>& found, Result& why)>;

struct Platform {
    std::string name;
    std::string charset;                              // used for random "characters" names
    std::function<bool(const std::string&)> valid;    // local rule check, saves wasted requests
    std::function<Result(const std::string&)> check;  // checks one name
    double delay = 1.0;                               // default seconds between requests
    std::string note;                                 // caveat shown before checking
    // Optional, for sites that can look up lots of names at once. Names `lookup` finds are taken.
    // The rest count as available, or go through `check` first if `confirm` is set. If a lookup
    // fails for any reason other than a rate limit, its names go through `check` one at a time.
    Lookup lookup = nullptr;
    size_t batch = 0;        // most names per lookup
    bool confirm = false;
    double lookup_gap = 0;   // least seconds between lookups, for sites that allow fewer of those
    // When set, a name the lookup can't find is only probably free: it counts as available but not
    // double-checked, with this as the reason.
    std::string unsure = "";
};

// Handles the answers every check treats the same: no response, and rate limits.
bool common_result(const Response& r, Result& out) {
    if (r.status == 0) {
        out = {State::Error, r.error};
        return true;
    }
    if (r.status == 429) {
        out = {State::RateLimited, "", retry_after_seconds(r)};
        return true;
    }
    return false;
}

std::string http_error(const Response& r) {
    return "HTTP " + std::to_string(r.status) + ": " + r.body.substr(0, 80);
}

// A profile page existing means taken, a 404 means probably free.
Result check_profile_url(const std::string& url) {
    Response r = http("GET", url);
    Result out;
    if (common_result(r, out)) return out;
    if (r.status == 204 || r.status == 404) return {State::Available, "no profile found"};
    if (r.status >= 200 && r.status < 300) return {State::Taken, ""};
    return {State::Error, "HTTP " + std::to_string(r.status)};
}

// Discord's sign-up check, the one that decides whether you can register a name.
Result discord_attempt(const std::string& u) {
    Response r = http("POST", DISCORD_API + "/unique-username/username-attempt-unauthed",
                      "{\"username\":" + json_quote(u) + "}");
    Result out;
    if (common_result(r, out)) return out;
    std::string value;
    if (r.status == 200 && json_get(r.body, "taken", value)) {
        return {value == "true" ? State::Taken : State::Available, ""};
    }
    if (json_get(r.body, "code", value) && value == "50035") {
        size_t errors = r.body.find("\"_errors\"");
        std::string msg = "invalid";
        if (errors != std::string::npos) json_get(r.body, "message", msg, errors);
        return {State::Invalid, msg};
    }
    return {State::Error, http_error(r)};
}

// One of Discord's endpoints, and when its rate limit ends.
struct Lane {
    std::chrono::steady_clock::time_point resting_until{};

    double left() const {
        return std::max(0.0, std::chrono::duration<double>(resting_until - std::chrono::steady_clock::now()).count());
    }
    bool open() const { return left() <= 0; }
    void rest(double seconds) {  // 0 means the server didn't say how long
        resting_until = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(seconds > 0 ? seconds + 0.5 : 15));
    }
};

Lane g_discord_suggest, g_discord_attempt;

// Discord has two public sign-up endpoints that can tell whether a name is free, each with its
// own rate limit (measured Sept 2026: ~37 checks, then a ~36 minute wait; ~20, then ~35 minutes):
//  - username-suggestions-unauthed hands back the name you asked for if it's free, or a variant
//    like "abcd." or "abcd0288" if it's taken. It allows more checks, so every name goes through
//    it first.
//  - username-attempt-unauthed (discord_attempt) is what sign-up enforces, so it's saved for
//    double-checking names that look free.
// While one is rate limited the other carries on alone. A name that looks free while the
// double-check is resting still counts as a hit, marked unconfirmed.
Result check_discord(const std::string& u) {
    bool looks_free = false;
    if (g_discord_suggest.open()) {
        Response r = http("GET", DISCORD_API + "/unique-username/username-suggestions-unauthed?global_name=" +
                                     url_encode(u));
        std::string suggestion;
        if (r.status == 429) {
            g_discord_suggest.rest(retry_after_seconds(r));
        } else if (r.status == 0) {
            return {State::Error, r.error};
        } else if (r.status == 200 && json_get(r.body, "username", suggestion)) {
            if (suggestion != u) return {State::Taken, ""};
            looks_free = true;
        } else {
            return {State::Error, http_error(r)};
        }
    }
    if (g_discord_attempt.open()) {
        Result res = discord_attempt(u);
        if (res.state != State::RateLimited) return res;
        g_discord_attempt.rest(res.retry_after);
    }
    if (looks_free) {
        Result res{State::Available, "not double-checked, main check is rate limited"};
        res.unconfirmed = true;
        return res;
    }
    return {State::RateLimited, "", std::min(g_discord_suggest.left(), g_discord_attempt.left())};
}

bool valid_discord(const std::string& u) {
    return u.size() >= 2 && u.size() <= 32 && only_chars(u, LOWER + DIGITS + "_.") &&
           u.find("..") == std::string::npos;
}

// Roblox's sign-up validation, so "available" means Roblox would let you register it.
Result check_roblox(const std::string& u) {
    Response r = http("GET", "https://auth.roblox.com/v1/usernames/validate?Username=" + url_encode(u) +
                                 "&Birthday=" + url_encode("2000-01-01T00:00:00.000Z"));
    Result out;
    if (common_result(r, out)) return out;
    std::string code, msg;
    if (!json_get(r.body, "code", code)) return {State::Error, http_error(r)};
    if (code == "0") return {State::Available, ""};
    if (code == "1") return {State::Taken, ""};
    if (!json_get(r.body, "message", msg)) msg = "code " + code;
    return {State::Invalid, msg};
}

// Roblox's user lookup takes 100 names per request and knows every account, banned ones included.
// It answers with the ones it found, as the name asked for and the account's current name.
bool lookup_roblox(const std::vector<std::string>& names, std::set<std::string>& found, Result& why) {
    Response r = http("POST", "https://users.roblox.com/v1/usernames/users",
                      "{\"usernames\":" + json_list(names) + ",\"excludeBannedUsers\":false}");
    if (common_result(r, why)) return false;
    if (r.status != 200 || r.body.find("\"data\"") == std::string::npos) {
        why = {State::Error, http_error(r)};
        return false;
    }
    json_names(r.body, "requestedUsername", found);
    json_names(r.body, "name", found);
    return true;
}

bool valid_roblox(const std::string& u) {
    return u.size() >= 3 && u.size() <= 20 && only_chars(u, LOWER + UPPER + DIGITS + "_") &&
           std::count(u.begin(), u.end(), '_') <= 1 && u.front() != '_' && u.back() != '_';
}

Options bearer(const std::string& token) {
    Options opt;
    opt.headers = {"Authorization: Bearer " + token};
    return opt;
}

// Decodes base64url, the encoding of a JWT token's middle part. Returns "" if it isn't that.
std::string base64url_decode(const std::string& s) {
    static const std::string digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned bits = 0, count = 0;
    for (char c : s) {
        if (c == '=') break;
        const size_t v = digits.find(c);
        if (v == std::string::npos) return "";
        bits = ((bits << 6) | static_cast<unsigned>(v)) & 0xFFFFFF;
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xFF);
        }
    }
    return out;
}

// Seconds until a token stops working, going by the "exp" in it if it's a JWT (Minecraft's are).
// Negative once it has, and huge when the token doesn't say (GitHub's don't).
double token_seconds_left(const std::string& token) {
    const size_t a = token.find('.'), b = token.find('.', a + 1);
    std::string exp;
    if (a == std::string::npos || b == std::string::npos ||
        !json_get(base64url_decode(token.substr(a + 1, b - a - 1)), "exp", exp)) {
        return 1e12;
    }
    try {
        return std::stod(exp) - static_cast<double>(std::time(nullptr));
    } catch (const std::exception&) {
        return 1e12;
    }
}

// Whether a saved token is there and hasn't run out.
bool token_works(const std::string& token) {
    return !token.empty() && token_seconds_left(token) > 60;
}

// Mojang's public profile lookup. It can't see names Minecraft is holding, see minecraft_platform.
Result check_minecraft(const std::string& u) {
    return check_profile_url("https://api.mojang.com/users/profiles/minecraft/" + url_encode(u));
}

// The check minecraft.net's name change page uses, so it needs a logged in Minecraft account's
// token. It's the only one that knows about names Minecraft is holding.
Result check_minecraft_signed_in(const std::string& u, const std::string& token) {
    Response r = http("GET", "https://api.minecraftservices.com/minecraft/profile/name/" + url_encode(u) + "/available",
                      "", bearer(token));
    Result out;
    if (common_result(r, out)) return out;
    if (r.status == 401) return {State::Error, "Minecraft token ran out or is wrong, paste a new one in Other apps"};
    std::string status;
    if (r.status == 200 && json_get(r.body, "status", status)) {
        if (status == "AVAILABLE") return {State::Available, ""};
        if (status == "DUPLICATE") return {State::Taken, ""};
        if (status == "NOT_ALLOWED") return {State::Invalid, "Minecraft doesn't allow it"};
    }
    return {State::Error, http_error(r)};
}

// Asks Minecraft whose `token` is. Returns "" and puts the player's name in `who` if it works.
std::string minecraft_token_problem(const std::string& token, std::string& who) {
    Response r = http("GET", "https://api.minecraftservices.com/minecraft/profile", "", bearer(token));
    if (r.status == 200 && json_get(r.body, "name", who)) return "";
    if (r.status == 0) return r.error;
    if (r.status == 401) return "Minecraft says that token is wrong or has run out";
    if (r.status == 404) return "that account doesn't have Minecraft Java";
    return http_error(r);
}

// Mojang's bulk profile lookup takes 10 names per request and answers with the ones that exist.
bool lookup_minecraft(const std::vector<std::string>& names, std::set<std::string>& found, Result& why) {
    Response r = http("POST", "https://api.minecraftservices.com/minecraft/profile/lookup/bulk/byname",
                      json_list(names));
    if (common_result(r, why)) return false;
    if (r.status != 200 || r.body.find('[') == std::string::npos) {
        why = {State::Error, http_error(r)};
        return false;
    }
    json_names(r.body, "name", found);
    return true;
}

bool valid_minecraft(const std::string& u) {
    return u.size() >= 3 && u.size() <= 16 && only_chars(u, LOWER + UPPER + DIGITS + "_");
}

Result check_github(const std::string& u) {
    return check_profile_url("https://github.com/" + url_encode(u));
}

// How long GitHub wants ringer to wait, or 0 if `r` isn't a rate limit. Running out of checks for
// the hour comes with a reset time, a short "slow down" limit comes with Retry-After, and when it
// says neither, GitHub's advice is to wait a minute. Both APIs can say so with a 403 or 429, and
// GraphQL also with a 200 and a RATE_LIMITED error. That last one is only looked for in GraphQL
// answers, which hold nothing but logins (no '_' allowed), not in REST ones, which hold bios.
double github_wait(const Response& r, bool graphql = false) {
    const bool limited =
        r.status == 429 ||
        (r.status == 403 && (!r.header("retry-after").empty() || r.header("x-ratelimit-remaining") == "0" ||
                             to_lower(r.body).find("rate limit") != std::string::npos)) ||
        (graphql && r.status == 200 && r.body.find("RATE_LIMITED") != std::string::npos);
    if (!limited) return 0;
    double wait = retry_after_seconds(r);
    if (wait <= 0 && r.header("x-ratelimit-remaining") == "0") {
        try {
            // A reset time, so a wrong PC clock skews it. Checks come back within the hour regardless.
            wait = std::min(std::stod(r.header("x-ratelimit-reset")) - static_cast<double>(std::time(nullptr)) + 1,
                            3600.0);
        } catch (const std::exception&) {
        }
    }
    return wait > 0 ? std::max(wait, 1.0) : 60;
}

// GitHub's REST API, one name at a time. With a token it allows 5,000 checks an hour (60 without
// one, which is why ringer only uses the API when there's a token).
Result check_github_api(const std::string& u, const std::string& token) {
    Response r = http("GET", "https://api.github.com/users/" + url_encode(u), "", bearer(token));
    const double wait = github_wait(r);
    if (wait > 0) return {State::RateLimited, "", wait};
    Result out;
    if (common_result(r, out)) return out;
    if (r.status == 404) return {State::Available, "no account found"};
    if (r.status == 200) return {State::Taken, ""};
    if (r.status == 401) return {State::Error, "GitHub rejected the token, set a new one in Other apps"};
    return {State::Error, http_error(r)};
}

// GitHub's GraphQL API asks about up to 100 names in one request, as
//   query { n_0: repositoryOwner(login: "abc") { login } n_1: ... }
// and the whole thing costs one point of the 5,000 an hour. repositoryOwner covers users and
// organizations and is null for a name nobody has. The names are safe to paste into the query
// because valid_github only lets through letters, numbers and '-'.
bool lookup_github(const std::vector<std::string>& names, std::set<std::string>& found, Result& why,
                   const std::string& token) {
    std::string query = "query {";
    for (size_t i = 0; i < names.size(); ++i) {
        query += " n_" + std::to_string(i) + ": repositoryOwner(login: \"" + names[i] + "\") { login }";
    }
    query += " }";
    Response r = http("POST", "https://api.github.com/graphql", "{\"query\":" + json_quote(query) + "}",
                      bearer(token));
    const double wait = github_wait(r, true);
    if (wait > 0) {
        why = {State::RateLimited, "", wait};
        return false;
    }
    if (common_result(r, why)) return false;
    const size_t data = r.body.find("\"data\"");
    if (r.status != 200 || data == std::string::npos) {
        why = {State::Error, http_error(r)};
        return false;
    }
    // Every name has to be in the answer, as an account or as null. If one's missing, something's
    // off, and checking this batch one name at a time beats calling names free that aren't.
    std::vector<std::string> taken;
    std::string value;
    for (size_t i = 0; i < names.size(); ++i) {
        if (!json_get(r.body, "n_" + std::to_string(i), value, data)) {
            why = {State::Error, "GitHub's answer left some names out"};
            return false;
        }
        if (value != "null") taken.push_back(to_lower(names[i]));
    }
    found.insert(taken.begin(), taken.end());
    return true;
}

// Asks GitHub whether `token` works, without using up a check. Returns "" if it does.
std::string github_token_problem(const std::string& token) {
    Response r = http("GET", "https://api.github.com/rate_limit", "", bearer(token));
    if (r.status == 200) return "";
    if (r.status == 0) return r.error;
    if (r.status == 401) return "GitHub says that token isn't valid";
    return http_error(r);
}

bool valid_github(const std::string& u) {
    return u.size() >= 1 && u.size() <= 39 && only_chars(u, LOWER + UPPER + DIGITS + "-") &&
           u.front() != '-' && u.back() != '-' && u.find("--") == std::string::npos;
}

bool is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Every one of `symbols` in `u` has a letter or number right after it, so no symbol at the end
// and no two in a row.
bool symbols_between(const std::string& u, const std::string& symbols) {
    for (size_t i = 0; i < u.size(); ++i) {
        if (symbols.find(u[i]) != std::string::npos && (i + 1 == u.size() || !is_alnum(u[i + 1]))) return false;
    }
    return true;
}

// Lichess asks for a full minute's wait after a 429.
Result lichess_patience(Result r) {
    if (r.state == State::RateLimited && r.retry_after <= 0) r.retry_after = 60;
    return r;
}

Result check_lichess(const std::string& u) {
    return lichess_patience(check_profile_url("https://lichess.org/api/user/" + url_encode(u)));
}

// Lichess takes 300 names per request and answers with the accounts it found. Closed accounts are
// in there too, since Lichess never gives a name out again.
bool lookup_lichess(const std::vector<std::string>& names, std::set<std::string>& found, Result& why) {
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "" : ",") + n;
    Options opt;
    opt.content_type = "text/plain";
    Response r = http("POST", "https://lichess.org/api/users", list, opt);
    if (common_result(r, why)) {
        why = lichess_patience(why);
        return false;
    }
    if (r.status != 200 || r.body.find('[') == std::string::npos) {
        why = {State::Error, http_error(r)};
        return false;
    }
    json_names(r.body, "id", found);
    return true;
}

// Lichess' sign-up rules: 2-20 characters, starting with a letter.
bool valid_lichess(const std::string& u) {
    return u.size() >= 2 && u.size() <= 20 && only_chars(u, LOWER + UPPER + DIGITS + "_-") &&
           (LOWER + UPPER).find(u.front()) != std::string::npos && symbols_between(u, "_-");
}

Lane g_chesscom_signup;
const double CHESSCOM_REST = 60;  // how long to rest the sign-up check when it doesn't say

// Chess.com's public API says whether an account exists, closed ones included, and it doesn't mind
// a steady stream of requests, so every name goes through it. Names with no account get
// double-checked with the sign-up form's check, which also knows about banned words. That one sits
// behind Cloudflare, though, and only takes a handful of checks before asking for a break
// (measured Sept 2026: 4 checks, then about a minute). While it's resting, hits are marked not
// double-checked.
Result check_chesscom(const std::string& u) {
    Result res = check_profile_url("https://api.chess.com/pub/player/" + url_encode(to_lower(u)));
    if (res.state != State::Available) return res;
    if (g_chesscom_signup.open()) {
        Response r = http("GET", "https://www.chess.com/callback/user/valid?username=" + url_encode(u));
        std::string valid, msg;
        if (r.status == 200 && json_get(r.body, "valid", valid)) {
            if (valid == "true") return {State::Available, ""};
            // "messages":["That username is taken. ..."]
            size_t list = r.body.find('[', r.body.find("\"messages\""));
            if (list != std::string::npos) json_string(r.body, r.body.find('"', list), msg);
            if (to_lower(msg).find("taken") != std::string::npos) return {State::Taken, ""};
            return {State::Invalid, msg.empty() ? "not allowed" : msg};
        }
        // Rate limited, a Cloudflare challenge, or something unexpected: give it a rest.
        const double asked = r.status == 429 ? retry_after_seconds(r) : 0;
        g_chesscom_signup.rest(asked > 0 ? asked : CHESSCOM_REST);
    }
    res.detail = "not double-checked, the sign-up check is resting";
    res.unconfirmed = true;
    return res;
}

// Chess.com's sign-up rules: 3-25 characters, starting with a letter or number.
bool valid_chesscom(const std::string& u) {
    return u.size() >= 3 && u.size() <= 25 && only_chars(u, LOWER + UPPER + DIGITS + "_-") && is_alnum(u.front()) &&
           symbols_between(u, "_-");
}

// The check GitLab's sign-up form uses. Users and groups share names on GitLab, so it covers
// both. Names GitLab keeps for itself (like "api" or "explore") redirect to the sign-in page.
// It allows about 20 checks a minute and its 429s don't say how long to wait.
Result check_gitlab(const std::string& u) {
    Options opt;
    opt.follow_redirects = false;
    Response r = http("GET", "https://gitlab.com/users/" + url_encode(u) + "/exists", "", opt);
    Result out;
    if (common_result(r, out)) return out;
    if (r.status >= 300 && r.status < 400) return {State::Invalid, "reserved by GitLab"};
    std::string exists;
    if (r.status == 200 && json_get(r.body, "exists", exists)) {
        return {exists == "true" ? State::Taken : State::Available, ""};
    }
    return {State::Error, http_error(r)};
}

bool valid_gitlab(const std::string& u) {
    const std::string lower = to_lower(u);
    auto ends_with = [&](const std::string& end) {
        return lower.size() >= end.size() && lower.compare(lower.size() - end.size(), end.size(), end) == 0;
    };
    return u.size() >= 2 && u.size() <= 255 && only_chars(u, LOWER + UPPER + DIGITS + "_.-") && is_alnum(u.front()) &&
           symbols_between(u, "_.-") && !ends_with(".git") && !ends_with(".atom");
}

Platform discord_platform() {
    return {"Discord", LOWER + DIGITS + "_.", valid_discord, check_discord, 1.5,
            "Checks each name with Discord's sign-up suggestions first, then double-checks anything "
            "that looks free with the check sign-up actually uses. Each has its own limit (roughly "
            "35-40 and 20 checks, then a wait of around half an hour), so together they get through "
            "2-3x as many names as before. While the main check is waiting, hits are marked "
            "\"not double-checked\" and the run keeps going."};
}

Platform roblox_platform() {
    Platform p{"Roblox", LOWER + DIGITS + "_", valid_roblox, check_roblox, 0.5,
               "Looks names up 100 at a time, then runs anything that isn't an existing account "
               "through Roblox's sign-up validation, so 'available' means Roblox would actually "
               "let you register it. Roblox only allows about one lookup every 7 seconds, so "
               "ringer spaces them out."};
    p.lookup = lookup_roblox;
    p.batch = 100;
    p.confirm = true;
    // From one connection the lookup gets a 429 about every 4th request at 5s apart, and says
    // nothing about how long to wait. 7s apart went 12 for 12 (measured Sept 2026).
    p.lookup_gap = 7;
    return p;
}

// Minecraft holds some names that no player has right now: ones changed away from in the last 37
// days, banned accounts' and old accounts' that were never moved to Microsoft. Mojang's public
// lookups say nobody has those, and with 3 characters almost every "free" one is held. Only the
// logged in check sees them, so without a Minecraft token hits are marked not double-checked.
Platform minecraft_platform(const std::string& token) {
    Platform p{"Minecraft (Java)", LOWER + DIGITS + "_", valid_minecraft, check_minecraft, 1.0, ""};
    p.lookup = lookup_minecraft;
    p.batch = 10;
    if (token_works(token)) {
        p.check = [token](const std::string& u) { return check_minecraft_signed_in(u, token); };
        p.confirm = true;
        p.note = "Looks names up 10 at a time, then double-checks any that no player has with the check "
                 "minecraft.net's name change page uses. That one knows about names Minecraft is holding "
                 "(changed in the last 37 days, banned, or never moved to Microsoft), so those count as taken.";
        return p;
    }
    p.unsure = "not double-checked, Minecraft may be holding it";
    p.check = [unsure = p.unsure](const std::string& u) {
        Result r = check_minecraft(u);
        if (r.state == State::Available) {
            r.detail = unsure;
            r.unconfirmed = true;
        }
        return r;
    };
    p.note = "Looks names up 10 at a time with Mojang's public lookup, which can't see names Minecraft is "
             "holding (changed in the last 37 days, banned, or never moved to Microsoft). With 3 characters "
             "nearly every hit is one of those. Hits are marked \"not double-checked\". Add a Minecraft token "
             "(Other apps > Minecraft token) to check them properly.";
    return p;
}

Platform github_platform(const std::string& token) {
    if (token.empty()) {
        return {"GitHub", LOWER + DIGITS + "-", valid_github, check_github, 1.5,
                "Checks whether the profile page exists. Reserved and deleted names can 404 but "
                "still can't be registered. Add a GitHub token (Other apps > GitHub token) to use "
                "GitHub's API instead, which looks names up 100 at a time."};
    }
    Platform p{"GitHub", LOWER + DIGITS + "-", valid_github,
               [token](const std::string& u) { return check_github_api(u, token); }, 0.75,
               "Uses GitHub's API with your token, looking names up 100 at a time. Reserved and "
               "deleted names come back as not found but still can't be registered."};
    p.lookup = [token](const std::vector<std::string>& names, std::set<std::string>& found, Result& why) {
        return lookup_github(names, found, why, token);
    };
    p.batch = 100;
    return p;
}

Platform lichess_platform() {
    Platform p{"Lichess", LOWER + DIGITS + "_-", valid_lichess, check_lichess, 1.0,
               "Looks names up 300 at a time. Closed accounts count as taken, because Lichess "
               "never frees a name. Lichess also turns down some offensive names at sign-up, "
               "which ringer can't see."};
    p.lookup = lookup_lichess;
    p.batch = 300;
    return p;
}

Platform chesscom_platform() {
    return {"Chess.com", LOWER + DIGITS + "_-", valid_chesscom, check_chesscom, 0.5,
            "Checks each name with Chess.com's public API, then double-checks names with no account "
            "using the sign-up form's check, which also catches banned words. Chess.com only allows "
            "a handful of those before it wants a break, so while it rests, hits are marked "
            "\"not double-checked\" and the run keeps going."};
}

Platform gitlab_platform() {
    return {"GitLab", LOWER + DIGITS + "_.-", valid_gitlab, check_gitlab, 3.0,
            "Uses the check GitLab's sign-up form does. Users and groups share names on GitLab, so "
            "a group's name counts as taken, and names GitLab keeps for its own pages show as not "
            "allowed. GitLab only allows about 20 of these checks a minute, hence the 3 seconds."};
}

// ---------------------------------------------------------------------------
// Text UI: every screen starts with a breadcrumb header, menus sit in boxes,
// and it's all laid out to fit an 80-column console.
// ---------------------------------------------------------------------------

std::string paint(const std::string& color, const std::string& text) {
    return color + text + RESET;
}

// Width of the UI in columns, leaving a margin so nothing wraps.
size_t ui_width() {
    return static_cast<size_t>(std::max(40, std::min(78, console_width() - 2)));
}

// How many columns `s` takes on screen, not counting color codes.
size_t visible_len(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033') {
            while (i < s.size() && s[i] != 'm') ++i;
        } else {
            ++n;
        }
    }
    return n;
}

std::string pad(const std::string& s, size_t width) {
    size_t len = visible_len(s);
    return len >= width ? s : s + std::string(width - len, ' ');
}

// Cuts plain text down to `width` columns, marking the cut with "..".
std::string fit(const std::string& s, size_t width) {
    if (s.size() <= width) return s;
    if (width < 3) return s.substr(0, width);
    return s.substr(0, width - 2) + "..";
}

std::vector<std::string> wrap(const std::string& text, size_t width) {
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word, line;
    while (words >> word) {
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            lines.push_back(line);
            line.clear();
        }
        while (word.size() > width) {
            lines.push_back(word.substr(0, width));
            word.erase(0, width);
        }
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

void print_lines(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) std::cout << " " << l << "\n";
}

// A box `width` columns wide around `lines`, which may carry colors:
//   +- title -------+
//   | line          |
//   +---------------+
std::vector<std::string> box(const std::string& title, const std::vector<std::string>& lines, size_t width) {
    const size_t inner = width - 4;
    std::string top = "+-";
    size_t used = 2;
    if (!title.empty()) {
        std::string t = fit(title, width - 6);
        top += std::string(RESET) + " " + BOLD + t + RESET + " " + DIM;
        used += t.size() + 2;
    }
    top += std::string(width - 1 - used, '-') + "+";

    std::vector<std::string> out = {paint(DIM, top)};
    const std::string side = paint(DIM, "|");
    for (const std::string& l : lines) {
        std::string shown = visible_len(l) > inner && l.find('\033') == std::string::npos ? fit(l, inner) : l;
        out.push_back(side + " " + pad(shown, inner) + " " + side);
    }
    out.push_back(paint(DIM, "+" + std::string(width - 2, '-') + "+"));
    return out;
}

// A box of plain text, word-wrapped to fit the screen.
void text_box(const std::string& title, const std::string& text) {
    print_lines(box(title, wrap(text, ui_width() - 4), ui_width()));
    std::cout << "\n";
}

std::string g_flash;  // a message for the top of the next screen, e.g. "Webhook saved."

void flash(const char* color, const std::string& msg) {
    g_flash = paint(color, fit(plain_text(msg), ui_width()));
}

// Clears the console and draws the header, a box saying where you are:
//   | ringer > Discord > Random 4 letters |
void screen(const std::vector<std::string>& crumbs) {
    clear_screen();
    const size_t width = ui_width();
    std::string plain, trail;
    for (size_t i = 0; i < crumbs.size(); ++i) {
        if (i) {
            plain += " > ";
            trail += paint(DIM, " > ");
        }
        plain += crumbs[i];
        trail += paint(i == 0 ? std::string(CYAN) + BOLD : i + 1 == crumbs.size() ? BOLD : "", crumbs[i]);
    }
    if (plain.size() > width - 4) trail = fit(plain, width - 4);
    print_lines(box("", {trail}, width));
    std::cout << "\n";
    if (!g_flash.empty()) {
        std::cout << " " << g_flash << "\n\n";
        g_flash.clear();
    }
}

std::string read_line(const std::string& prompt) {
    std::cout << " " << prompt << std::flush;
    std::string s;
    if (!std::getline(std::cin, s)) {
        std::cout << "\n";
        std::exit(0);
    }
    return trim(s);
}

void say_error(const std::string& msg) {
    std::cout << " " << paint(RED, msg) << "\n";
}

bool is_number(const std::string& s) {
    return !s.empty() && s.size() <= 9 && only_chars(s, DIGITS);
}

struct Option {
    std::string label;
    std::string hint;  // shown on the right, may carry colors
};

// A numbered menu in a box. Options are 1..n and `back`, if given, is 0 at the bottom.
std::vector<std::string> menu_box(const std::string& title, const std::vector<Option>& options,
                                  const std::string& back, size_t width) {
    const size_t inner = width - 4;
    std::vector<std::string> lines;
    auto add = [&](const std::string& key, const Option& o) {
        std::string label = fit(o.label, inner - key.size() - 4);
        std::string line = paint(CYAN, "[" + key + "]") + "  " + label;
        size_t used = key.size() + 4 + label.size(), hint = visible_len(o.hint);
        if (hint && used + 2 + hint <= inner) line += std::string(inner - used - hint, ' ') + paint(DIM, o.hint);
        lines.push_back(line);
    };
    for (size_t i = 0; i < options.size(); ++i) add(std::to_string(i + 1), options[i]);
    if (!back.empty()) {
        lines.push_back("");
        add("0", {back, ""});
    }
    return box(title, lines, width);
}

// Reads a menu pick: 1..count, or 0 if the menu has a back option.
int read_choice(size_t count, bool has_back) {
    for (;;) {
        std::string raw = read_line(paint(CYAN, "> "));
        if (is_number(raw)) {
            size_t n = std::stoul(raw);
            if ((n >= 1 && n <= count) || (n == 0 && has_back)) return static_cast<int>(n);
        }
        say_error("Type a number from " + std::to_string(has_back ? 0 : 1) + " to " + std::to_string(count) + ".");
    }
}

// Draws a menu and returns the pick: 1..options.size(), or 0 for `back`.
int menu(const std::string& title, const std::vector<Option>& options, const std::string& back = "") {
    size_t width = title.size() + 8;
    for (const Option& o : options) {
        width = std::max(width, o.label.size() + (o.hint.empty() ? 0 : visible_len(o.hint) + 2) + 9);
    }
    width = std::min(std::max<size_t>(width, 44), ui_width());
    print_lines(menu_box(title, options, back, width));
    return read_choice(options.size(), !back.empty());
}

int ask_int(const std::string& prompt, int def, int lo, int hi) {
    for (;;) {
        std::string raw = read_line(prompt + " " + paint(DIM, "[" + std::to_string(def) + "]") + ": ");
        if (raw.empty()) return def;
        if (is_number(raw)) {
            int n = std::stoi(raw);
            if (n >= lo && n <= hi) return n;
        }
        say_error("Enter a whole number from " + std::to_string(lo) + " to " + std::to_string(hi) + ".");
    }
}

std::string format_seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", s);
    return buf;
}

double ask_seconds(const std::string& prompt, double def) {
    for (;;) {
        std::string raw = read_line(prompt + " " + paint(DIM, "[" + format_seconds(def) + "]") + ": ");
        if (raw.empty()) return def;
        try {
            size_t used = 0;
            double v = std::stod(raw, &used);
            if (used == raw.size() && v >= 0 && v <= 3600) return v;
        } catch (const std::exception&) {
        }
        say_error("Enter a number like 0.5 or 2.");
    }
}

// 42s, 3m 07s, 2h 05m, 3d 4h
std::string duration_text(double seconds) {
    long s = std::lround(std::max(0.0, seconds));
    char buf[32];
    if (s < 60) std::snprintf(buf, sizeof buf, "%lds", s);
    else if (s < 3600) std::snprintf(buf, sizeof buf, "%ldm %02lds", s / 60, s % 60);
    else if (s < 86400) std::snprintf(buf, sizeof buf, "%ldh %02ldm", s / 3600, s / 60 % 60);
    else std::snprintf(buf, sizeof buf, "%ldd %ldh", s / 86400, s / 3600 % 24);
    return buf;
}

// The local time `seconds` from now, like "14:05".
std::string clock_in(double seconds) {
    std::time_t t = std::time(nullptr) + static_cast<std::time_t>(std::ceil(seconds));
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M", &local);
    return buf;
}

// ---------------------------------------------------------------------------
// Settings (and the Discord webhook)
// ---------------------------------------------------------------------------

struct Config {
    std::string webhook;
    std::string mention;       // "", "@everyone", or a Discord user ID
    std::string github_token;     // makes GitHub checks use the API, see check_github_api
    std::string minecraft_token;  // lets Minecraft hits get double-checked, see minecraft_platform
    // App name -> when its last long rate limit ends (unix time), so a restart knows about it.
    std::map<std::string, long long> limited_until;
};

Config load_config() {
    Config cfg;
    std::ifstream f(CONFIG_FILE);
    if (!f.is_open()) f.open(OLD_CONFIG_FILE);  // settings saved before the rename
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "webhook") {
            cfg.webhook = value;
        } else if (key == "mention") {
            cfg.mention = value;
        } else if (key == "github_token") {
            cfg.github_token = value;
        } else if (key == "minecraft_token") {
            cfg.minecraft_token = value;
        } else if (starts_with(key, "limited_until.")) {
            try {
                cfg.limited_until[key.substr(14)] = std::stoll(value);
            } catch (const std::exception&) {
            }
        }
    }
    return cfg;
}

bool save_config(const Config& cfg) {
    std::ofstream f(CONFIG_FILE);
    f << "webhook=" << cfg.webhook << "\n" << "mention=" << cfg.mention << "\n";
    if (!cfg.github_token.empty()) f << "github_token=" << cfg.github_token << "\n";
    if (!cfg.minecraft_token.empty()) f << "minecraft_token=" << cfg.minecraft_token << "\n";
    const long long now = std::time(nullptr);
    for (const auto& app : cfg.limited_until) {
        if (app.second > now) f << "limited_until." << app.first << "=" << app.second << "\n";
    }
    f.close();
    return !f.fail();
}

// Seconds left on the last long rate limit `app` gave us, 0 if there isn't one.
double limit_left(const Config& cfg, const std::string& app) {
    auto it = cfg.limited_until.find(app);
    if (it == cfg.limited_until.end()) return 0;
    return static_cast<double>(std::max<long long>(0, it->second - static_cast<long long>(std::time(nullptr))));
}

// Menu hint for an app that's still rate limiting us, like "limited 31m 05s".
std::string limit_hint(const Config& cfg, const std::string& app, const std::string& otherwise = "") {
    double left = limit_left(cfg, app);
    return left > 0 ? paint(YELLOW, "limited " + duration_text(left)) : otherwise;
}

bool looks_like_webhook(const std::string& url) {
    for (const char* host : {"https://discord.com/api/", "https://discordapp.com/api/",
                             "https://ptb.discord.com/api/", "https://canary.discord.com/api/"}) {
        if (starts_with(url, host) && url.find("/webhooks/") != std::string::npos) return true;
    }
    return false;
}

std::string mention_label(const Config& cfg) {
    if (cfg.mention.empty()) return "nobody, it just posts the name";
    if (cfg.mention == "@everyone") return "@everyone";
    return "user " + cfg.mention;
}

// Posts a message to the webhook, waiting out rate limits. Returns "" on success.
std::string webhook_send(const Config& cfg, const std::string& text) {
    std::string content = text, allowed = "{\"parse\":[]}";
    if (cfg.mention == "@everyone") {
        content = "@everyone " + text;
        allowed = "{\"parse\":[\"everyone\"]}";
    } else if (!cfg.mention.empty()) {
        content = "<@" + cfg.mention + "> " + text;
        allowed = "{\"users\":[" + json_quote(cfg.mention) + "]}";
    }
    const std::string body = "{\"username\":\"ringer\",\"content\":" + json_quote(content) +
                             ",\"allowed_mentions\":" + allowed + "}";
    for (int attempt = 0; attempt < 5 && !g_stop; ++attempt) {
        Response r = http("POST", cfg.webhook, body);
        if (r.status >= 200 && r.status < 300) return "";
        if (r.status == 429) {
            double wait = retry_after_seconds(r);
            nap(wait > 0 ? wait : 5);
            continue;
        }
        if (r.status == 0) return r.error;
        if (r.status == 401 || r.status == 404) return "webhook was deleted or the URL is wrong";
        return http_error(r);
    }
    return "still rate limited after 5 tries";
}

// Asks who to ping. Returns false if they backed out.
bool pick_mention(Config& cfg) {
    int who = menu("Who gets pinged when a name is found?",
                   {{"Nobody", "just post the name"}, {"@everyone", ""}, {"One person", "by user ID"}}, "Back");
    if (who == 0) return false;
    if (who == 1) {
        cfg.mention.clear();
    } else if (who == 2) {
        cfg.mention = "@everyone";
    } else {
        std::cout << "\n " << paint(DIM, "Discord: Settings > Advanced > turn on Developer Mode, then right-click") << "\n"
                  << " " << paint(DIM, "the person > Copy User ID.") << "\n";
        for (;;) {
            std::string id = read_line("User ID (blank to go back): ");
            if (id.empty()) return false;
            if (id.size() >= 15 && id.size() <= 21 && only_chars(id, DIGITS)) {
                cfg.mention = id;
                break;
            }
            say_error("That's not a user ID, it should be 17-20 digits.");
        }
    }
    return true;
}

void set_webhook_url(Config& cfg) {
    screen({"ringer", "Webhook pings", "Set URL"});
    text_box("Where to get one",
             "In Discord: channel settings > Integrations > Webhooks > New Webhook > Copy Webhook URL. "
             "Treat it like a password, anyone who has it can post in that channel.");
    std::string url;
    for (;;) {
        url = read_line("Webhook URL (blank to go back): ");
        if (url.empty()) return;
        if (looks_like_webhook(url)) break;
        say_error("That doesn't look like a Discord webhook URL.");
    }
    Config trial = cfg;
    trial.webhook = url;
    std::cout << "\n";
    if (!pick_mention(trial)) return;
    std::cout << "\n " << paint(DIM, "Sending a test message...") << std::flush;
    std::string err = webhook_send(trial, "ringer is hooked up. Available usernames will show up here.");
    if (!err.empty()) {
        flash(RED, "Didn't work: " + err);
        return;
    }
    cfg = trial;
    if (save_config(cfg)) flash(GREEN, "Webhook saved. Check your channel for the test message.");
    else flash(YELLOW, "The webhook works, but ringer couldn't save it to " + std::string(CONFIG_FILE) + ".");
}

void webhook_settings(Config& cfg) {
    for (;;) {
        screen({"ringer", "Webhook pings"});
        const bool on = !cfg.webhook.empty();
        print_lines(box("Discord webhook", {
            paint(DIM, "Status   ") + (on ? paint(GREEN, "on") : "off"),
            paint(DIM, "Pings    ") + (on ? mention_label(cfg) : "-"),
        }, ui_width()));
        std::cout << "\n";

        int pick = menu("What do you want to do?",
                        {{"Set webhook URL", ""}, {"Change who gets pinged", ""}, {"Send a test message", ""},
                         {"Turn off", ""}},
                        "Back");
        if (pick == 0) return;
        if (pick == 1) {
            set_webhook_url(cfg);
        } else if (!on && (pick == 2 || pick == 3)) {
            flash(YELLOW, "Set a webhook URL first.");
        } else if (pick == 2) {
            screen({"ringer", "Webhook pings", "Who gets pinged"});
            Config changed = cfg;
            if (!pick_mention(changed)) continue;
            cfg = changed;
            if (save_config(cfg)) flash(GREEN, "Saved.");
            else flash(YELLOW, "Couldn't save " + std::string(CONFIG_FILE) + ".");
        } else if (pick == 3) {
            std::cout << "\n " << paint(DIM, "Sending...") << std::flush;
            std::string err = webhook_send(cfg, "ringer test message.");
            if (err.empty()) flash(GREEN, "Sent. Check your channel.");
            else flash(RED, "Didn't work: " + err);
        } else {
            cfg.webhook.clear();
            cfg.mention.clear();
            if (save_config(cfg)) flash(GREEN, "Webhook turned off.");
            else flash(YELLOW, "Couldn't save " + std::string(CONFIG_FILE) + ".");
        }
    }
}

// ---------------------------------------------------------------------------
// Picking the platform and usernames
// ---------------------------------------------------------------------------

// Asks for a profile URL template. Returns false if they backed out.
bool custom_platform(Platform& out) {
    screen({"ringer", "Other apps", "Custom site"});
    const size_t inner = ui_width() - 4;
    std::vector<std::string> lines = wrap("Give ringer a profile URL with {username} where the name goes, like:", inner);
    lines.push_back("  " + paint(CYAN, "https://example.com/users/{username}"));
    lines.push_back("");
    lines.push_back("A 404 (page not found) counts as available, a 200 as taken.");
    print_lines(box("How it works", lines, ui_width()));
    std::cout << "\n";

    std::string tmpl;
    for (;;) {
        tmpl = read_line("URL (blank to go back): ");
        if (tmpl.empty()) return false;
        if (tmpl.find("{username}") != std::string::npos &&
            (starts_with(tmpl, "http://") || starts_with(tmpl, "https://"))) {
            break;
        }
        say_error("Needs to start with http(s):// and contain {username}.");
    }

    std::string host = tmpl.substr(tmpl.find("://") + 3);
    host = plain_text(host.substr(0, host.find('/')));

    auto check = [tmpl](const std::string& u) {
        std::string url = tmpl;
        size_t pos;
        while ((pos = url.find("{username}")) != std::string::npos) url.replace(pos, 10, url_encode(u));
        return check_profile_url(url);
    };
    auto valid = [](const std::string& u) { return only_chars(u, LOWER + UPPER + DIGITS + "_.-"); };
    out = {host.empty() ? "Custom" : host, LOWER + DIGITS, valid, check, 1.0,
           "Plenty of sites return 200 for every URL, or 404 for banned names. "
           "Test it with a name you KNOW exists first."};
    return true;
}

// A login token that gives an app a better check, and the screen for setting it.
struct TokenScreen {
    std::string app;                // "GitHub"
    std::string* token;             // where it's kept in the config
    std::string with, without;      // what that app's checks use with a token and without
    std::string where;              // how to get one
    std::string chars;              // characters a token can have
    // Asks the app whether a token works. Returns "" if it does, and may say whose it is in `who`.
    std::function<std::string(const std::string&, std::string&)> test;
};

// "on", "expired", or how long it has left if the token says.
std::string token_status(const std::string& token) {
    if (token.empty()) return "off";
    const double left = token_seconds_left(token);
    if (left <= 60) return paint(YELLOW, "ran out, set a new one");
    if (left < 1e9) return paint(GREEN, "on") + ", works for another " + duration_text(left);
    return paint(GREEN, "on");
}

std::string token_hint(const std::string& token) {
    if (token.empty()) return "off";
    return token_works(token) ? paint(GREEN, "on") : paint(YELLOW, "ran out");
}

void token_settings(Config& cfg, const TokenScreen& t) {
    for (;;) {
        screen({"ringer", "Other apps", t.app + " token"});
        const bool on = token_works(*t.token);
        print_lines(box(t.app + " token", {
            paint(DIM, "Status   ") + token_status(*t.token),
            paint(DIM, "Checks   ") + (on ? t.with : t.without),
        }, ui_width()));
        std::cout << "\n";

        int pick = menu("What do you want to do?", {{"Set token", ""}, {"Remove token", ""}}, "Back");
        if (pick == 0) return;
        if (pick == 2) {
            if (t.token->empty()) {
                flash(YELLOW, "There's no token to remove.");
                continue;
            }
            t.token->clear();
            if (save_config(cfg)) flash(GREEN, "Token removed. " + t.app + " checks use " + t.without + " again.");
            else flash(YELLOW, "Couldn't save " + std::string(CONFIG_FILE) + ".");
            continue;
        }

        screen({"ringer", "Other apps", t.app + " token", "Set token"});
        text_box("Where to get one", t.where);
        std::string token;
        for (;;) {
            token = read_line("Token (blank to go back): ");
            // Copied from a request's Authorization header, it can start with "Bearer ".
            if (to_lower(token).rfind("bearer ", 0) == 0) token = trim(token.substr(7));
            if (token.empty() || only_chars(token, t.chars)) break;
            say_error("That doesn't look like a " + t.app + " token.");
        }
        if (token.empty()) continue;
        std::cout << "\n " << paint(DIM, "Checking it with " + t.app + "...") << std::flush;
        std::string who;
        std::string problem = t.test(token, who);
        if (!problem.empty()) {
            flash(RED, "Didn't work: " + problem);
            continue;
        }
        *t.token = token;
        const std::string whose = who.empty() ? "" : " for " + who;
        if (save_config(cfg)) flash(GREEN, "Token saved" + whose + ". " + t.app + " checks use " + t.with + " now.");
        else flash(YELLOW, "The token works, but ringer couldn't save it to " + std::string(CONFIG_FILE) + ".");
    }
}

TokenScreen github_token_screen(Config& cfg) {
    return {"GitHub", &cfg.github_token, "the API", "profile pages",
            "On github.com: Settings > Developer settings > Personal access tokens > Fine-grained "
            "tokens > Generate new token. It doesn't need any permissions, the defaults are fine. "
            "Treat it like a password, ringer saves it in " + std::string(CONFIG_FILE) + ".",
            LOWER + UPPER + DIGITS + "_",
            [](const std::string& token, std::string&) { return github_token_problem(token); }};
}

TokenScreen minecraft_token_screen(Config& cfg) {
    return {"Minecraft", &cfg.minecraft_token, "the logged in check", "the public lookup",
            "Log in on minecraft.net and open your profile page. Press F12, open the Network tab and "
            "reload the page. Click a request to api.minecraftservices.com and copy what comes after "
            "\"Bearer \" in its Authorization header. It works for about a day. Until then anyone who "
            "has it can change your Minecraft name and skin, so don't share it. ringer saves it in " +
                std::string(CONFIG_FILE) + ".",
            LOWER + UPPER + DIGITS + "_-.", minecraft_token_problem};
}

// Returns false if they backed out to the main menu.
bool pick_other_app(Config& cfg, Platform& out) {
    for (;;) {
        screen({"ringer", "Other apps"});
        const bool token = !cfg.github_token.empty();
        const bool signed_in = token_works(cfg.minecraft_token);
        int pick = menu("Which app?",
                        {{"Minecraft (Java)", limit_hint(cfg, "Minecraft (Java)", signed_in ? "10 per request" : "public lookup")},
                         {"GitHub", limit_hint(cfg, "GitHub", token ? "100 per request" : "profile page")},
                         {"Lichess", limit_hint(cfg, "Lichess", "300 per request")},
                         {"Chess.com", limit_hint(cfg, "Chess.com", "sign-up check")},
                         {"GitLab", limit_hint(cfg, "GitLab", "sign-up check")},
                         {"Custom site", "any profile URL"},
                         {"GitHub token", token_hint(cfg.github_token)},
                         {"Minecraft token", token_hint(cfg.minecraft_token)}},
                        "Back");
        switch (pick) {
            case 0: return false;
            case 1: out = minecraft_platform(cfg.minecraft_token); return true;
            case 2: out = github_platform(cfg.github_token); return true;
            case 3: out = lichess_platform(); return true;
            case 4: out = chesscom_platform(); return true;
            case 5: out = gitlab_platform(); return true;
            case 6:
                if (custom_platform(out)) return true;
                break;
            case 7: token_settings(cfg, github_token_screen(cfg)); break;
            default: token_settings(cfg, minecraft_token_screen(cfg));
        }
    }
}

std::vector<std::string> random_names(const std::string& charset, int length, int count, const Platform& p) {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> pick(0, charset.size() - 1);
    const double space = std::pow(static_cast<double>(charset.size()), length);

    std::vector<std::string> names;
    std::set<std::string> seen;
    for (long attempts = 0; static_cast<int>(names.size()) < count && attempts < 50L * count &&
                            static_cast<double>(seen.size()) < space;
         ++attempts) {
        std::string name;
        for (int i = 0; i < length; ++i) name += charset[pick(rng)];
        if (!seen.insert(name).second || !p.valid(name)) continue;
        names.push_back(name);
    }
    return names;
}

std::vector<std::string> names_from_file(const std::string& path, const Platform& p, bool lowercase, int& skipped) {
    std::ifstream f(path);
    std::vector<std::string> names;
    std::set<std::string> seen;
    std::string line;
    bool first = true;
    skipped = 0;
    while (std::getline(f, line)) {
        if (first && starts_with(line, "\xEF\xBB\xBF")) line.erase(0, 3);  // Notepad's UTF-8 BOM
        first = false;
        std::string name = trim(line);
        if (name.empty() || name[0] == '#') continue;
        if (lowercase) name = to_lower(name);
        if (!seen.insert(name).second) continue;
        if (!p.valid(name)) {
            ++skipped;
            continue;
        }
        names.push_back(name);
    }
    return names;
}

// "a-z 0-9 _ ." for a charset, to show what "characters" means on each app.
std::string charset_hint(const std::string& charset) {
    std::string hint, rest;
    if (charset.find('a') != std::string::npos) hint += "a-z";
    if (charset.find('0') != std::string::npos) hint += hint.empty() ? "0-9" : " 0-9";
    for (char c : charset) {
        if (LOWER.find(c) == std::string::npos && DIGITS.find(c) == std::string::npos) {
            rest += ' ';
            rest += c;
        }
    }
    return hint + rest;
}

struct Job {
    std::string label;                  // what's being checked, for the header
    std::vector<std::string> names;
    std::vector<std::string> warnings;  // shown when the run starts
    double delay = 1.0;
    bool from_file = false;             // stopped runs save what's left so it can be picked up later
};

// Asks what to check and how fast. Returns false if they backed out to the main menu.
bool pick_job(const Platform& p, const Config& cfg, Job& job) {
    enum Kind { File, Letters, Chars, Custom };
    struct Mode {
        const char* label;
        const char* hint;
        Kind kind;
        int length;
    };
    const std::vector<Mode> modes = {
        {"From a .txt file", "one name per line", File, 0},
        {"Random 3 letters", "abc", Letters, 3},
        {"Random 3 characters", "a1b", Chars, 3},
        {"Random 4 letters", "abcd", Letters, 4},
        {"Random 4 characters", "a1b2", Chars, 4},
        {"Random 5 letters", "abcde", Letters, 5},
        {"Random 5 characters", "a1b2c", Chars, 5},
        {"Random, pick the length", "", Custom, 0},
    };
    std::vector<Option> options;
    for (const Mode& m : modes) options.push_back({m.label, m.hint});

    for (;;) {
        screen({"ringer", p.name});
        int pick = menu("What kind of usernames?", options, "Back");
        if (pick == 0) return false;
        Mode mode = modes[pick - 1];
        job = Job{};
        job.label = mode.label;

        screen({"ringer", p.name, job.label});
        text_box("Heads up", p.note);
        const double left = limit_left(cfg, p.name);
        if (left > 0) {
            std::cout << " " << paint(YELLOW, fit(p.name + " was rate limiting you until " + clock_in(left) + ", " +
                                                      duration_text(left) + " from now.", ui_width()))
                      << "\n " << paint(DIM, "On the same connection the run will likely have to wait for that.")
                      << "\n\n";
        }

        if (mode.kind == File) {
            std::cout << " " << paint(DIM, "One name per line. Drag the file into this window or type its path.") << "\n";
            std::string path;
            for (;;) {
                path = read_line("File (blank to go back): ");
                while (!path.empty() && (path.front() == '"' || path.front() == '\'')) path.erase(0, 1);
                while (!path.empty() && (path.back() == '"' || path.back() == '\'')) path.pop_back();
                if (path.empty() || std::ifstream(path)) break;
                say_error("Can't find that file.");
            }
            if (path.empty()) continue;
            // Discord usernames are lowercase only, so lowercase them for the user.
            int skipped = 0;
            job.names = names_from_file(path, p, p.name == "Discord", skipped);
            if (job.names.empty()) {
                flash(RED, "That file has no names " + p.name + " would allow.");
                continue;
            }
            job.from_file = true;
            if (skipped) {
                job.warnings.push_back("Skipped " + std::to_string(skipped) + " name(s) that break " + p.name +
                                       "'s username rules.");
            }
        } else {
            if (mode.kind == Custom) {
                mode.length = ask_int("Length", 6, 1, 32);
                std::cout << "\n";
                int pick_chars = menu("Using", {{"Letters only", "a-z"},
                                                {"Letters, numbers and symbols", charset_hint(p.charset)}},
                                      "Back");
                if (pick_chars == 0) continue;
                mode.kind = pick_chars == 1 ? Letters : Chars;
                job.label = "Random " + std::to_string(mode.length) + (mode.kind == Letters ? " letters" : " characters");
                std::cout << "\n";
            }
            int count = ask_int("How many to check", 100, 1, 1000000);
            job.names = random_names(mode.kind == Letters ? LOWER : p.charset, mode.length, count, p);
            if (job.names.empty()) {
                flash(RED, p.name + " doesn't allow " + std::to_string(mode.length) + "-character names.");
                continue;
            }
            if (static_cast<int>(job.names.size()) < count) {
                job.warnings.push_back("Only " + std::to_string(job.names.size()) +
                                       " names of that type are possible, checking all of them.");
            }
        }
        std::cout << "\n " << paint(DIM, "Lower is faster, but gets rate limited more.") << "\n";
        job.delay = ask_seconds("Seconds between requests", p.delay);
        return true;
    }
}

// ---------------------------------------------------------------------------
// The checking loop
// ---------------------------------------------------------------------------

void save_hit(const std::string& platform, const std::string& name, bool unconfirmed) {
    std::ofstream f(RESULTS_FILE, std::ios::app);
    f << platform << ": " << name << (unconfirmed ? " (not double-checked)" : "") << "\n";
}

// The live progress line under a run's results:
//  [#########..................]   25%  25/100  2 found  37s  ~1m 51s left
//  [#########..................]   25%  25/100  2 found  rate limited: 12s
struct ProgressBar {
    size_t total, done = 0, found = 0;
    std::string waiting;  // shown instead of the time left while set, e.g. during a rate limit
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    size_t width = ui_width();
    size_t bar;           // columns inside the brackets, fixed so the bar doesn't jump around
    std::string shown;    // what's on screen now, so identical redraws can be skipped

    // The time left goes by requests, not names. A batch lookup answers 100 names at once and then
    // waits before the next request, so "time so far per name" swings all over the place. Instead
    // it's (requests still needed) x (seconds each one has taken, waits between included). It's
    // worked out just before each request goes out, when every name the last one answered has been
    // counted and the wait before it is over, and counts down in between. Rate limit waits don't
    // count and pause the countdown, since there's no telling if another one's coming.
    size_t requests = 0;         // requests that got an answer, so not rate limited
    double request_seconds = 0;  // how long those took, without the waits between them
    double paused = 0;           // seconds spent waiting out rate limits
    double eta = -1;      // seconds left as of eta_at, -1 until there's anything to go on
    std::chrono::steady_clock::time_point eta_at;

    explicit ProgressBar(size_t n) : total(n) {
        // Leave room for the longest text this many names can produce.
        const int digits = static_cast<int>(std::to_string(total).size());
        const int text = 6 + (2 * digits + 3) + (digits + 8) + 9 + 15;
        bar = static_cast<size_t>(std::max(10, std::min(40, static_cast<int>(width) - 3 - text)));
    }

    double elapsed() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    void update_eta() {
        if (!requests || !done) return;
        const double n = static_cast<double>(requests);
        const double left = static_cast<double>(total - done) / (static_cast<double>(done) / n);  // requests
        // The next request goes out now, with no wait before it, and there's no wait after the last.
        eta = std::max(0.0, left - 1) * ((elapsed() - paused) / n) + std::min(left, 1.0) * (request_seconds / n);
        eta_at = std::chrono::steady_clock::now();
    }

    double eta_left() const {
        return std::max(0.0, eta - std::chrono::duration<double>(std::chrono::steady_clock::now() - eta_at).count());
    }

    std::string render() const {
        const size_t filled = total ? bar * done / total : 0;
        std::string line = " [" + paint(CYAN, std::string(filled, '#')) + paint(DIM, std::string(bar - filled, '.')) + "]";
        size_t used = bar + 3;
        auto add = [&](const std::string& s, const std::string& color) {
            if (used + 2 + s.size() > width + 1) return;
            line += "  " + paint(color, s);
            used += 2 + s.size();
        };
        char pct[8];
        std::snprintf(pct, sizeof pct, "%3d%%", static_cast<int>(total ? 100 * done / total : 0));
        add(pct, BOLD);
        add(std::to_string(done) + "/" + std::to_string(total), "");
        add(std::to_string(found) + " found", found ? GREEN : "");
        if (!waiting.empty()) {
            add(waiting, YELLOW);
        } else {
            add(duration_text(elapsed()), DIM);
            if (done && done < total && eta >= 0) add("~" + duration_text(eta_left()) + " left", DIM);
        }
        return line + std::string(width + 1 - used, ' ');
    }

    void draw() {
        std::string line = render();
        if (line == shown) return;
        std::cout << "\r" << line << std::flush;
        shown = line;
    }

    // Prints a line above the bar.
    void print(const std::string& line) {
        std::cout << "\r" << std::string(width + 1, ' ') << "\r" << line << "\n";
        shown.clear();
        draw();
    }

    void finish() {
        shown.clear();
        draw();
        std::cout << "\n";
    }
};

struct Tally {
    size_t checked = 0, taken = 0, invalid = 0, errors = 0, rate_limits = 0, pings = 0;
    std::vector<std::string> found;  // unconfirmed ones end in '?'
    size_t unconfirmed = 0;
    double seconds = 0, rate_waited = 0;
    bool stopped = false;
    std::string webhook_error;
    size_t unchecked_saved = 0;  // names written to UNCHECKED_FILE
    double check_gap = 0;        // where one-name checks' spacing settled, see run()
};

void print_summary(const Platform& p, size_t total, const Tally& t, bool webhook) {
    const size_t width = ui_width(), key = 12, room = width - 6 - key;
    auto count = [](size_t n, const std::string& what, const std::string& color) {
        return paint(n ? color : DIM, std::to_string(n) + " " + what);
    };
    std::vector<std::string> lines = {
        "",
        "  " + count(t.found.size(), "available", std::string(GREEN) + BOLD) + "    " + count(t.taken, "taken", RED) +
            "    " + count(t.invalid, "not allowed", YELLOW) + "    " +
            count(t.errors, t.errors == 1 ? "error" : "errors", YELLOW),
        "",
    };
    auto row = [&](const std::string& k, const std::string& v) { lines.push_back("  " + paint(DIM, pad(k, key)) + v); };

    row("App", p.name);
    row("Checked", std::to_string(t.checked) + " of " + std::to_string(total) + " names in " + duration_text(t.seconds));
    if (t.check_gap > 0) {
        row("Paced", "one check every " + format_seconds(std::round(t.check_gap * 10) / 10) + "s, to stay under " +
                         p.name + "'s limit");
    }
    if (t.rate_limits) {
        row("Slowed by", std::to_string(t.rate_limits) + " rate limit" + (t.rate_limits == 1 ? "" : "s") + ", waited " +
                             duration_text(t.rate_waited));
    }
    if (webhook) {
        if (t.webhook_error.empty()) row("Webhook", std::to_string(t.pings) + " ping" + (t.pings == 1 ? "" : "s") + " sent");
        else row("Webhook", paint(YELLOW, fit("stopped after an error: " + t.webhook_error, room)));
    }

    if (t.found.empty()) {
        row("Found", paint(DIM, "nothing this time"));
    } else {
        // Names wrapped into up to 5 lines, the rest are in the results file anyway.
        std::vector<std::string> rows(1);
        size_t listed = 0;
        for (const std::string& name : t.found) {
            std::string n = fit(name, room);
            if (!rows.back().empty() && rows.back().size() + 2 + n.size() > room) {
                if (rows.size() == 5) break;
                rows.emplace_back();
            }
            rows.back() += (rows.back().empty() ? "" : "  ") + n;
            ++listed;
        }
        for (size_t i = 0; i < rows.size(); ++i) row(i ? "" : "Found", paint(std::string(GREEN) + BOLD, rows[i]));
        if (listed < t.found.size()) row("", paint(DIM, "and " + std::to_string(t.found.size() - listed) + " more"));
        if (t.unconfirmed) {
            row("", paint(DIM, fit("? = not double-checked, " + std::to_string(t.unconfirmed) + " of them", room)));
        }
        row("Saved to", RESULTS_FILE);
    }
    if (t.unchecked_saved) {
        row("Left over", std::to_string(t.unchecked_saved) + " name" + (t.unchecked_saved == 1 ? "" : "s") +
                             " saved to " + UNCHECKED_FILE);
        row("", paint(DIM, fit(std::string("load ") + UNCHECKED_FILE + " next time to carry on", room)));
    }
    lines.push_back("");
    print_lines(box(t.stopped ? "Stopped early" : "Done", lines, width));
}

void run(const Platform& p, const Job& job, Config& cfg) {
    using namespace std::chrono;
    screen({"ringer", p.name, job.label, "Checking"});
    const size_t total = job.names.size();
    bool use_webhook = !cfg.webhook.empty();
    std::cout << " Checking " << paint(BOLD, std::to_string(total)) << " name" << (total == 1 ? "" : "s") << " on "
              << p.name << ", " << format_seconds(job.delay) << "s apart. " << paint(DIM, "Ctrl+C stops early.") << "\n";
    if (p.lookup) {
        std::string how = "Up to " + std::to_string(p.batch) + " names per request";
        if (p.lookup_gap > job.delay) how += ", at most one every " + format_seconds(p.lookup_gap) + "s (" + p.name + "'s limit)";
        std::cout << " " << paint(DIM, how + ".") << "\n";
    }
    std::cout << " " << paint(DIM, std::string("Hits get saved to ") + RESULTS_FILE +
                                       (use_webhook ? " and posted to your Discord webhook." : "."))
              << "\n";
    for (const std::string& w : job.warnings) std::cout << " " << paint(YELLOW, w) << "\n";
    std::cout << "\n";

    Tally t;
    g_stop = false;
    g_checking = true;
    ProgressBar bar(total);
    const std::string of_total = "/" + std::to_string(total);
    const size_t digits = std::to_string(total).size();
    bar.draw();

    // Waits out a rate limit with a countdown on the bar. Long ones get a heads-up with the time it
    // carries on, and get saved to ringer.cfg so the menus can warn about them after a restart.
    auto wait_out = [&](double seconds) {
        ++t.rate_limits;
        if (seconds >= 60) {
            cfg.limited_until[p.name] = static_cast<long long>(std::time(nullptr)) + static_cast<long long>(std::ceil(seconds));
            save_config(cfg);
            bar.print(" " + paint(YELLOW, fit(p.name + " rate limited you for " + duration_text(seconds) +
                                                  ". Carrying on by itself at " + clock_in(seconds) + ".",
                                              bar.width)));
            bar.print(" " + paint(DIM, fit("Leave this window open, or Ctrl+C to stop and see what it found so far.", bar.width)));
        }
        const auto began = steady_clock::now();
        const auto until = began + duration_cast<steady_clock::duration>(duration<double>(seconds));
        nap(seconds, [&] {
            double left = duration<double>(until - steady_clock::now()).count();
            bar.waiting = "rate limited: " + duration_text(std::ceil(std::max(0.0, left)));
            bar.draw();
        });
        t.rate_waited += duration<double>(steady_clock::now() - began).count();
        bar.paused += duration<double>(steady_clock::now() - began).count();
        bar.eta_at += steady_clock::now() - began;  // the time left doesn't count down while waiting
        bar.waiting.clear();
    };
    // For sites that rate limit without saying how long: 15s, then 30s, 1m... up to 10m.
    double backoff = 15;
    auto wait_for = [&](const Result& limited) {
        if (limited.retry_after > 0) {
            wait_out(limited.retry_after + 0.5);
        } else {
            wait_out(backoff);
            backoff = std::min(backoff * 2, 600.0);
        }
    };

    // One-name checks on sites that rate limit without saying for how long (Minecraft's logged in
    // check, Roblox's validation, GitLab, Chess.com) get spacing that adapts. Each 429 stretches it
    // by half, starting at twice the run's delay (1s at least) and going up to a minute, and waits
    // that long before trying again. Each check that goes through shrinks it by 3%. So it settles
    // just under the site's real limit, instead of running into it and waiting 15s, 30s, 1m...
    double check_gap = 0;

    // Keeps requests job.delay apart, checks check_gap apart and lookups p.lookup_gap apart too,
    // counting from when the last one finished. Names a batch lookup already answered don't need a
    // request, so they don't wait. The time left gets worked out once the wait is over.
    steady_clock::time_point last_request, last_lookup, asked;
    bool sent = false, looked = false;
    auto wait_since = [&](steady_clock::time_point since, double seconds) {
        nap(seconds - duration<double>(steady_clock::now() - since).count(), [&] { bar.draw(); });
    };
    auto pace = [&](bool lookup) {
        if (sent) wait_since(last_request, lookup ? job.delay : std::max(job.delay, check_gap));
        if (lookup && looked) wait_since(last_lookup, p.lookup_gap);
        bar.update_eta();
        asked = steady_clock::now();
    };
    auto sent_now = [&](bool lookup, bool answered) {
        sent = true;
        last_request = steady_clock::now();
        if (lookup) {
            looked = true;
            last_lookup = last_request;
        }
        if (answered) {
            ++bar.requests;
            bar.request_seconds += duration<double>(last_request - asked).count();
        }
    };

    std::set<std::string> found;  // lowercased names the current batch lookup found
    size_t looked_up = 0;         // names before this index have had a batch lookup
    bool lookup_failed = false;   // the current batch gets checked one name at a time instead

    for (size_t i = 0; i < total && !g_stop; ++i) {
        const std::string& name = job.names[i];
        if (p.lookup && i == looked_up) {
            const std::vector<std::string> batch(job.names.begin() + i, job.names.begin() + std::min(total, i + p.batch));
            looked_up += batch.size();
            found.clear();
            Result why;
            for (;;) {
                pace(true);
                lookup_failed = !p.lookup(batch, found, why);
                sent_now(true, !lookup_failed || why.state != State::RateLimited);
                if (!lookup_failed || why.state != State::RateLimited || g_stop) break;
                wait_for(why);
                if (g_stop) break;
            }
            if (g_stop) break;
            if (lookup_failed) {
                bar.print(" " + paint(YELLOW, fit("Batch lookup failed (" + plain_text(why.detail) +
                                                      "), checking these one at a time.",
                                                  bar.width)));
            }
        }

        Result res{State::Error, ""};
        const bool batched = p.lookup && !lookup_failed;
        if (batched && found.count(to_lower(name))) {
            res = {State::Taken, ""};
        } else if (batched && !p.confirm) {
            res = {State::Available, p.unsure};
            res.unconfirmed = !p.unsure.empty();
        } else {
            for (;;) {
                pace(false);
                res = p.check(name);
                sent_now(false, res.state != State::RateLimited);
                if (res.state != State::RateLimited || g_stop) break;
                if (res.retry_after > 0) {
                    wait_for(res);
                } else {
                    check_gap = std::min(60.0, check_gap > 0 ? check_gap * 1.5 : std::max(1.0, 2 * job.delay));
                    wait_out(check_gap);
                }
                if (g_stop) break;
            }
            if (res.state != State::RateLimited) check_gap *= 0.97;
        }
        if (g_stop) break;
        backoff = 15;

        ++t.checked;
        if (res.state == State::Available) {
            t.found.push_back(res.unconfirmed ? name + "?" : name);
            if (res.unconfirmed) ++t.unconfirmed;
        }
        bar.done = t.checked;
        bar.found = t.found.size();

        std::string counter = std::to_string(i + 1);
        counter = std::string(digits - counter.size(), ' ') + counter + of_total;
        auto report = [&](const std::string& color, const std::string& label, const std::string& detail) {
            std::string line = " " + paint(DIM, counter) + "  " + paint(color, pad(label, 11)) + "  " + name;
            const size_t used = 1 + counter.size() + 2 + 11 + 2 + name.size();
            std::string d = plain_text(detail);
            if (!d.empty() && used + 4 + 12 <= bar.width + 1) line += "  " + paint(DIM, "(" + fit(d, bar.width - used - 3) + ")");
            bar.print(line);
        };

        switch (res.state) {
            case State::Available: {
                save_hit(p.name, name, res.unconfirmed);
                report(std::string(GREEN) + BOLD, "AVAILABLE", res.detail);
                std::cout << "\a" << std::flush;
                if (use_webhook) {
                    std::string err = webhook_send(cfg, "\xE2\x9C\x85 `" + name + "` is available on **" + p.name + "**" +
                                                            (res.unconfirmed ? " (not double-checked)" : ""));
                    if (err.empty()) {
                        ++t.pings;
                    } else {
                        t.webhook_error = plain_text(err);
                        use_webhook = false;
                        bar.print(" " + paint(YELLOW, fit("Webhook failed (" + t.webhook_error + "), skipping it for the rest of this run.",
                                                          bar.width)));
                    }
                }
                break;
            }
            case State::Taken:
                ++t.taken;
                report(RED, "taken", "");
                break;
            case State::Invalid:
                ++t.invalid;
                report(YELLOW, "not allowed", res.detail);
                break;
            default:
                ++t.errors;
                report(YELLOW, "error", res.detail);
        }
        bar.draw();
    }

    bar.finish();
    t.seconds = bar.elapsed();
    t.check_gap = check_gap > job.delay ? check_gap : 0;
    t.stopped = g_stop;
    g_checking = false;
    g_stop = false;

    // Names are checked in order, so everything from t.checked on is still to do.
    if (t.stopped && job.from_file && t.checked < total) {
        std::ofstream f(UNCHECKED_FILE);
        for (size_t i = t.checked; i < total; ++i) f << job.names[i] << "\n";
        f.close();
        if (!f.fail()) t.unchecked_saved = total - t.checked;
    }

    std::cout << "\n";
    print_summary(p, total, t, !cfg.webhook.empty());
    std::cout << "\n";
}

// ---------------------------------------------------------------------------
// Every app: a few names, each checked on all of them
// ---------------------------------------------------------------------------

std::vector<Platform> every_platform(const Config& cfg) {
    return {discord_platform(), roblox_platform(), minecraft_platform(cfg.minecraft_token), github_platform(cfg.github_token),
            lichess_platform(), chesscom_platform(), gitlab_platform()};
}

// Names typed with spaces or commas between them. An '@' in front is fine, and repeats (ignoring
// case) only count once.
std::vector<std::string> split_names(std::string text) {
    std::replace(text.begin(), text.end(), ',', ' ');
    std::istringstream words(text);
    std::vector<std::string> names;
    std::set<std::string> seen;
    std::string word;
    while (words >> word) {
        while (!word.empty() && word.front() == '@') word.erase(0, 1);
        if (!word.empty() && seen.insert(to_lower(word)).second) names.push_back(word);
    }
    return names;
}

// How a name did on each app.
struct NameResult {
    std::string name;
    std::vector<std::string> free;  // apps it's free on, unconfirmed ones end in '?'
    bool partial = false;           // Ctrl+C stopped it before every app was checked
};

void print_everywhere_summary(const std::vector<NameResult>& results, size_t apps, double seconds,
                              const std::vector<std::string>& skipped, bool webhook, size_t pings,
                              const std::string& webhook_error, bool stopped) {
    const size_t width = ui_width();
    size_t longest = 8;
    for (const NameResult& r : results) longest = std::max(longest, r.name.size());
    const size_t key = std::min<size_t>(longest, 16) + 2, room = width - 6 - key;
    std::vector<std::string> lines = {""};
    auto row = [&](const std::string& k, const std::string& v) {
        lines.push_back("  " + paint(DIM, pad(fit(k, key - 2), key)) + v);
    };

    row("Checked", std::to_string(results.size()) + " name" + (results.size() == 1 ? "" : "s") + " on " +
                       std::to_string(apps) + " apps in " + duration_text(seconds));
    lines.push_back("");
    bool unconfirmed = false, found = false;
    for (const NameResult& r : results) {
        std::string text;
        std::string color = GREEN;
        if (r.free.empty()) {
            text = "free nowhere";
            color = DIM;
        } else if (r.free.size() == apps &&
                   std::none_of(r.free.begin(), r.free.end(), [](const std::string& f) { return f.back() == '?'; })) {
            text = "free everywhere";
            color = std::string(GREEN) + BOLD;
        } else {
            text = "free on ";
            for (size_t i = 0; i < r.free.size(); ++i) text += (i ? ", " : "") + r.free[i];
        }
        if (r.partial) text += " (stopped partway)";
        for (const std::string& f : r.free) unconfirmed = unconfirmed || f.back() == '?';
        found = found || !r.free.empty();
        std::vector<std::string> wrapped = wrap(text, room);
        for (size_t i = 0; i < wrapped.size(); ++i) {
            lines.push_back("  " + std::string(i ? "" : BOLD) + pad(i ? "" : fit(r.name, key - 2), key) +
                            (i ? "" : RESET) + paint(color, wrapped[i]));
        }
    }
    lines.push_back("");
    if (unconfirmed) row("", paint(DIM, "? = not double-checked"));
    for (const std::string& note : skipped) row("Skipped", paint(YELLOW, fit(note, room)));
    if (webhook) {
        if (webhook_error.empty()) row("Webhook", std::to_string(pings) + " ping" + (pings == 1 ? "" : "s") + " sent");
        else row("Webhook", paint(YELLOW, fit("stopped after an error: " + webhook_error, room)));
    }
    if (found) row("Saved to", RESULTS_FILE);
    lines.push_back("");
    print_lines(box(stopped ? "Stopped early" : "Done", lines, width));
}

// Checks each name on every app, showing each answer as it comes in. An app that makes ringer wait
// a minute or less gets waited out. One that wants longer is skipped until then, so a long Discord
// wait doesn't hold up the other six.
void check_everywhere(const std::vector<std::string>& names, Config& cfg) {
    using namespace std::chrono;
    screen({"ringer", "Every app", "Checking"});
    struct App {
        Platform p;
        steady_clock::time_point last{};  // when its last request finished
        bool sent = false;
        Lane resting{};                   // skipped until this runs out
    };
    std::vector<App> apps;
    for (const Platform& p : every_platform(cfg)) apps.push_back({p});
    bool use_webhook = !cfg.webhook.empty();
    const size_t width = ui_width();
    size_t column = 0;  // app names line up in a column this wide
    for (const App& a : apps) column = std::max(column, a.p.name.size() + 2);
    std::cout << " Checking " << paint(BOLD, std::to_string(names.size())) << " name" << (names.size() == 1 ? "" : "s")
              << " on " << apps.size() << " apps. " << paint(DIM, "Ctrl+C stops early.") << "\n"
              << " " << paint(DIM, std::string("Hits get saved to ") + RESULTS_FILE +
                                       (use_webhook ? " and posted to your Discord webhook." : "."))
              << "\n\n";

    // Redraws the line for one app, like "   Roblox       checking...".
    auto draw = [&](const std::string& app, const std::string& status) {
        std::string line = "   " + pad(app, column) + status;
        std::cout << "\r" << line << std::string(width > visible_len(line) ? width - visible_len(line) : 0, ' ')
                  << std::flush;
    };
    auto answer = [&](const std::string& app, const std::string& color, const std::string& label,
                      const std::string& detail) {
        std::string line = paint(color, pad(label, 12));
        const size_t used = 3 + column + 12;
        std::string d = plain_text(detail);
        if (!d.empty() && used + 4 + 12 <= width) line += "  " + paint(DIM, "(" + fit(d, width - used - 4) + ")");
        draw(app, line);
        std::cout << "\n";
    };

    g_stop = false;
    g_checking = true;
    const auto start = steady_clock::now();
    std::vector<NameResult> results;
    std::vector<std::string> skipped;
    size_t pings = 0;
    std::string webhook_error;

    for (const std::string& name : names) {
        if (g_stop) break;
        std::cout << " " << paint(BOLD, fit(name, width - 1)) << "\n";
        NameResult nr;
        nr.name = name;
        bool unconfirmed = false;
        for (App& a : apps) {
            if (g_stop) break;
            const Platform& p = a.p;
            // Discord names are lowercase only, so that's the one Discord gets asked about.
            const std::string u = p.name == "Discord" ? to_lower(name) : name;
            if (!p.valid(u)) {
                answer(p.name, YELLOW, "not allowed", "breaks " + p.name + "'s username rules");
                continue;
            }
            if (!a.resting.open()) {
                answer(p.name, DIM, "skipped", "rate limited until " + clock_in(a.resting.left()));
                continue;
            }
            draw(p.name, paint(DIM, "checking..."));
            Result res;
            for (int waits = 0;; ++waits) {
                if (a.sent) nap(p.delay - duration<double>(steady_clock::now() - a.last).count());
                res = p.check(u);
                a.sent = true;
                a.last = steady_clock::now();
                if (res.state != State::RateLimited || g_stop) break;
                const double wait = res.retry_after > 0 ? res.retry_after + 0.5 : 15;
                if (wait > 60 || waits == 2) {
                    const double rest = res.retry_after > 0 ? res.retry_after : 60;
                    a.resting.rest(rest);
                    if (rest >= 60) {
                        cfg.limited_until[p.name] =
                            static_cast<long long>(std::time(nullptr)) + static_cast<long long>(std::ceil(rest));
                        save_config(cfg);
                    }
                    skipped.push_back(p.name + " from " + name + " on, rate limited until " + clock_in(rest));
                    break;
                }
                const auto until = steady_clock::now() + duration_cast<steady_clock::duration>(duration<double>(wait));
                nap(wait, [&] {
                    const double left = duration<double>(until - steady_clock::now()).count();
                    draw(p.name, paint(YELLOW, "rate limited, waiting " + duration_text(std::ceil(std::max(0.0, left)))));
                });
                if (g_stop) break;
                draw(p.name, paint(DIM, "checking..."));
            }
            if (g_stop) {
                draw("", "");
                std::cout << "\r";
                break;
            }
            switch (res.state) {
                case State::Available:
                    answer(p.name, std::string(GREEN) + BOLD, "AVAILABLE", res.detail);
                    save_hit(p.name, u, res.unconfirmed);
                    nr.free.push_back(res.unconfirmed ? p.name + "?" : p.name);
                    unconfirmed = unconfirmed || res.unconfirmed;
                    break;
                case State::Taken: answer(p.name, RED, "taken", ""); break;
                case State::Invalid: answer(p.name, YELLOW, "not allowed", res.detail); break;
                case State::RateLimited:
                    answer(p.name, YELLOW, "rate limited", "skipping it until " + clock_in(a.resting.left()));
                    break;
                default: answer(p.name, YELLOW, "error", res.detail);
            }
        }
        nr.partial = g_stop;
        if (!nr.free.empty()) {
            std::cout << "\a" << std::flush;
            if (use_webhook) {
                std::string where;
                for (size_t i = 0; i < nr.free.size(); ++i) {
                    std::string app = nr.free[i];
                    if (app.back() == '?') app.pop_back();
                    where += std::string(i ? ", " : "") + "**" + app + "**";
                }
                std::string err = webhook_send(cfg, "\xE2\x9C\x85 `" + name + "` is available on " + where +
                                                        (unconfirmed ? " (some not double-checked)" : ""));
                if (err.empty()) {
                    ++pings;
                } else {
                    webhook_error = plain_text(err);
                    use_webhook = false;
                    std::cout << " " << paint(YELLOW, fit("Webhook failed (" + webhook_error + "), skipping it from now on.", width))
                              << "\n";
                }
            }
        }
        results.push_back(nr);
        std::cout << "\n";
    }

    const bool stopped = g_stop;
    g_checking = false;
    g_stop = false;
    print_everywhere_summary(results, apps.size(), duration<double>(steady_clock::now() - start).count(), skipped,
                             !cfg.webhook.empty(), pings, webhook_error, stopped);
    std::cout << "\n";
}

// The Every app screen. Returns false if they chose to quit.
bool every_app(Config& cfg) {
    for (;;) {
        screen({"ringer", "Every app"});
        text_box("How it works",
                 "Type a name, or a few with spaces between them, and ringer checks each one on Discord, "
                 "Roblox, Minecraft, GitHub, Lichess, Chess.com and GitLab. Keep it to a handful: Discord "
                 "only allows about 20 checks before a long wait, and an app that wants ringer to wait "
                 "more than a minute gets skipped for the rest of the run.");
        std::string limited;
        for (const Platform& p : every_platform(cfg)) {
            const double left = limit_left(cfg, p.name);
            if (left > 0) limited += (limited.empty() ? "" : ", ") + p.name + " until " + clock_in(left);
        }
        if (!limited.empty()) {
            for (const std::string& line :
                 wrap("Still rate limiting you: " + limited + ". Those will probably be skipped.", ui_width() - 2)) {
                std::cout << " " << paint(YELLOW, line) << "\n";
            }
            std::cout << "\n";
        }
        const std::vector<std::string> names = split_names(read_line("Names (blank to go back): "));
        if (names.empty()) return true;
        std::cout << "\n";
        check_everywhere(names, cfg);
        int next = menu("What next?", {{"Check more names on every app", ""}, {"Main menu", ""}}, "Quit");
        if (next == 0) return false;
        if (next == 2) return true;
    }
}

// ---------------------------------------------------------------------------
// The main screen
// ---------------------------------------------------------------------------

// Saturn in the style of https://scipython.com/media/old_blog/ascii-art/saturn-ascii-i.png,
// shaded with the usual 70-step ramp of ASCII characters from dense ($@B%8&WM#) to
// sparse (,"^`'.). SATURN_PAINT says which character is planet (o) and which is ring (=).
const char* const SATURN[] = {
    R"(                         ';>-}1)))}_l)",
    R"(                    'i]\nvrjrftf\jzXXv-)",
    R"(                 !}jnnzJLwh*****amj{cXX+)",
    R"(             '<\YLQLOwZZqCzzCp*****Y{XX{)",
    R"(           !(Uqahbh*#aqQUv]'  _h***01XX;)",
    R"(        '-xUZdho#&%%WapmZOJ/:  w**o(cX-)",
    R"(      `}vrzqoM&B@@8MakbbbwCn)^)**ajuz~)",
    R"(     -vrzhwpoMWWM*oaoo*adOYntzo*qfcr;)",
    R"(   ;rcfq*oX0wdkhao#MM*hq0UzJk*hzrv-)",
    R"(  ~zuja**):Jmdka**ohbw0CJOk*bYrv}`)",
    R"( -Xc(o**w  !uLOZmZOQLCZdo*Zcnx-')",
    R"(;XX10***h_  '_fucYQwbo*mYxu(!)",
    R"({XX{Y*****pCzzCqa**bLXxu|<')",
    R"(+XXc{jma*****hwLJznnj}!)",
    R"( -vXXzj\ftfrjrvn\]i')",
    R"(   l_})))1}->;')",
};
const char* const SATURN_PAINT[] = {
    "                         ============",
    "                    ===================",
    "                 =======================",
    "             ===ooooooo=================",
    "           ==ooooooooooooooo  ==========",
    "        ====ooooooooooooooooo  ========",
    "      =====ooooooooooooooooooo========",
    "     ======oooooooooooooooooo========",
    "   =======oooooooooooooooooo=======",
    "  ========oooooooooooooooo========",
    " ========  oooooooooooo=========",
    "==========  ooooooo==========",
    "===========================",
    "=======================",
    " ===================",
    "   ============",
};
const size_t SATURN_WIDTH = 40;

const char* const TITLE[] = {
    R"(         _)",
    R"(   _____(_)___  ____ ____  _____)",
    R"(  / ___/ / __ \/ __ `/ _ \/ ___/)",
    R"( / /  / / / / / /_/ /  __/ /)",
    R"(/_/  /_/_/ /_/\__, /\___/_/)",
    R"(             /____/)",
};

std::string saturn_line(size_t row) {
    const std::string art = SATURN[row], paint_map = SATURN_PAINT[row];
    std::string out;
    char current = ' ';
    for (size_t i = 0; i < art.size(); ++i) {
        char part = i < paint_map.size() ? paint_map[i] : '=';
        if (art[i] != ' ' && part != current) {
            out += part == 'o' ? YELLOW : GOLD;
            current = part;
        }
        out += art[i];
    }
    return out + RESET;
}

// Draws the main screen and returns the menu pick.
int home_screen(const Config& cfg) {
    screen({"ringer"});
    const std::vector<Option> options = {
        {"Discord", limit_hint(cfg, "Discord")},
        {"Roblox", limit_hint(cfg, "Roblox")},
        {"Other apps", ""},
        {"Every app", "all 7 at once"},
        {"Webhook pings", cfg.webhook.empty() ? "off" : paint(GREEN, "on")},
    };

    std::vector<std::string> right;
    for (const char* line : TITLE) right.push_back(paint(std::string(CYAN) + BOLD, line));
    right.push_back(paint(DIM, "      find unclaimed usernames"));
    right.push_back("");

    const size_t width = ui_width(), gap = 3;
    if (width >= SATURN_WIDTH + gap + 34) {
        std::vector<std::string> m = menu_box("Pick an app", options, "Quit", width - SATURN_WIDTH - gap);
        right.insert(right.end(), m.begin(), m.end());
        const size_t rows = std::max(right.size(), sizeof SATURN / sizeof SATURN[0]);
        for (size_t i = 0; i < rows; ++i) {
            std::string left = i < sizeof SATURN / sizeof SATURN[0] ? saturn_line(i) : "";
            std::cout << " " << pad(left, SATURN_WIDTH) << std::string(gap, ' ') << (i < right.size() ? right[i] : "")
                      << "\n";
        }
    } else {
        // Too narrow for Saturn, just the title and the menu.
        std::vector<std::string> m = menu_box("Pick an app", options, "Quit", std::min<size_t>(width, 44));
        right.insert(right.end(), m.begin(), m.end());
        print_lines(right);
    }
    std::cout << "\n";
    return read_choice(options.size(), true);
}

// The main menu. Returns false when they quit.
bool pick_platform(Config& cfg, Platform& out) {
    for (;;) {
        switch (home_screen(cfg)) {
            case 0: return false;
            case 1: out = discord_platform(); return true;
            case 2: out = roblox_platform(); return true;
            case 3:
                if (pick_other_app(cfg, out)) return true;
                break;
            case 4:
                if (!every_app(cfg)) return false;
                break;
            default: webhook_settings(cfg);
        }
    }
}

}  // namespace

int main() {
    setup_console();
    std::signal(SIGINT, on_sigint);
    Config cfg = load_config();

    Platform p;
    while (pick_platform(cfg, p)) {
        Job job;
        while (pick_job(p, cfg, job)) {
            run(p, job, cfg);
            int next = menu("What next?", {{"Check more names on " + p.name, ""}, {"Main menu", ""}}, "Quit");
            if (next == 0) return 0;
            if (next == 2) break;
        }
    }
    return 0;
}
