// usercheck: find unclaimed usernames on Discord, Roblox and a few other sites.
//
// Windows uses WinHTTP, which is built into Windows, so there's nothing to install.
// Linux and macOS use libcurl.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <set>
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
#endif

namespace {

const char* USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0 Safari/537.36";
const char* RESULTS_FILE = "available.txt";
const char* CONFIG_FILE = "usercheck.cfg";

const char* GREEN = "\033[92m";
const char* RED = "\033[91m";
const char* YELLOW = "\033[93m";
const char* CYAN = "\033[96m";
const char* DIM = "\033[2m";
const char* BOLD = "\033[1m";
const char* RESET = "\033[0m";

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

// Sleeps, but wakes up early if Ctrl+C is pressed.
void nap(double seconds) {
    using namespace std::chrono;
    auto end = steady_clock::now() + duration_cast<steady_clock::duration>(duration<double>(seconds));
    while (!g_stop && steady_clock::now() < end) {
        std::this_thread::sleep_for(std::min<steady_clock::duration>(milliseconds(100), end - steady_clock::now()));
    }
}

void setup_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
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

    out.clear();
    if (doc[i] != '"') {
        size_t end = doc.find_first_of(",}] \t\r\n", i);
        out = doc.substr(i, end == std::string::npos ? std::string::npos : end - i);
        return true;
    }
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

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

struct Response {
    long status = 0;          // 0 means no answer at all, see `error`
    std::string body;
    std::string retry_after;  // Retry-After header, if the server sent one
    std::string error;
};

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

Response http(const std::string& method, const std::string& url, const std::string& body = "") {
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

    std::wstring wurl = widen(url);
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

    std::wstring headers = L"Accept: application/json, text/html\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";
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

    wchar_t retry[64];
    size = sizeof(retry);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CUSTOM, L"Retry-After", retry, &size, WINHTTP_NO_HEADER_INDEX)) {
        for (DWORD i = 0; i < size / sizeof(wchar_t); ++i) r.retry_after += static_cast<char>(retry[i]);
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
    auto* retry_after = static_cast<std::string*>(userdata);
    std::string line(p, size * n);
    std::string lower = to_lower(line);
    if (starts_with(lower, "http/")) retry_after->clear();  // new response after a redirect
    else if (starts_with(lower, "retry-after:")) *retry_after = trim(line.substr(12));
    return size * n;
}

Response http(const std::string& method, const std::string& url, const std::string& body = "") {
    Response r;
    static CURL* curl = [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return curl_easy_init();
    }();
    if (!curl) {
        r.error = "couldn't start libcurl";
        return r;
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &r.retry_after);

    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json, text/html");
    if (method == "POST") {
        headers = curl_slist_append(headers, "Content-Type: application/json");
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
double retry_after_seconds(const Response& r) {
    std::string value;
    double seconds = 10.0;
    try {
        if (json_get(r.body, "retry_after", value)) seconds = std::stod(value);
        else if (!r.retry_after.empty()) seconds = std::stod(r.retry_after);
    } catch (const std::exception&) {
    }
    return std::max(seconds, 1.0);
}

// ---------------------------------------------------------------------------
// Platforms
// ---------------------------------------------------------------------------

enum class State { Available, Taken, Invalid, Error, RateLimited };

struct Result {
    State state;
    std::string detail;
    double retry_after = 0;
};

struct Platform {
    std::string name;
    std::string charset;                              // used for random "characters" names
    std::function<bool(const std::string&)> valid;    // local rule check, saves wasted requests
    std::function<Result(const std::string&)> check;
    double delay;                                     // default seconds between requests
    std::string note;                                 // caveat shown before checking
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

Result check_discord(const std::string& u) {
    Response r = http("POST", "https://discord.com/api/v9/unique-username/username-attempt-unauthed",
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

bool valid_discord(const std::string& u) {
    return u.size() >= 2 && u.size() <= 32 && only_chars(u, LOWER + DIGITS + "_.") &&
           u.find("..") == std::string::npos;
}

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

bool valid_roblox(const std::string& u) {
    return u.size() >= 3 && u.size() <= 20 && only_chars(u, LOWER + UPPER + DIGITS + "_") &&
           std::count(u.begin(), u.end(), '_') <= 1 && u.front() != '_' && u.back() != '_';
}

Result check_minecraft(const std::string& u) {
    return check_profile_url("https://api.mojang.com/users/profiles/minecraft/" + url_encode(u));
}

bool valid_minecraft(const std::string& u) {
    return u.size() >= 3 && u.size() <= 16 && only_chars(u, LOWER + UPPER + DIGITS + "_");
}

Result check_github(const std::string& u) {
    return check_profile_url("https://github.com/" + url_encode(u));
}

bool valid_github(const std::string& u) {
    return u.size() >= 1 && u.size() <= 39 && only_chars(u, LOWER + UPPER + DIGITS + "-") &&
           u.front() != '-' && u.back() != '-' && u.find("--") == std::string::npos;
}

Platform discord_platform() {
    return {"Discord", LOWER + DIGITS + "_.", valid_discord, check_discord, 1.5,
            "Uses the same public check as Discord's sign-up page. "
            "Discord rate limits this hard, the checker waits it out automatically."};
}

Platform roblox_platform() {
    return {"Roblox", LOWER + DIGITS + "_", valid_roblox, check_roblox, 0.5,
            "Uses Roblox's sign-up validation, so 'available' means Roblox "
            "would actually let you register it."};
}

Platform minecraft_platform() {
    return {"Minecraft (Java)", LOWER + DIGITS + "_", valid_minecraft, check_minecraft, 1.0,
            "Checks for an existing profile. Names that were just changed are "
            "locked for a while and banned names also show as free."};
}

Platform github_platform() {
    return {"GitHub", LOWER + DIGITS + "-", valid_github, check_github, 1.5,
            "Checks whether the profile page exists. Reserved and deleted "
            "names can 404 but still can't be registered."};
}

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

std::string read_line(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string s;
    if (!std::getline(std::cin, s)) {
        std::cout << "\n";
        std::exit(0);
    }
    return trim(s);
}

bool is_number(const std::string& s) {
    return !s.empty() && s.size() <= 9 && only_chars(s, DIGITS);
}

// Returns the 0-based index of the picked option.
size_t ask_choice(const std::string& title, const std::vector<std::string>& options) {
    std::cout << "\n" << BOLD << title << RESET << "\n";
    for (size_t i = 0; i < options.size(); ++i) {
        std::cout << "  " << CYAN << "[" << i + 1 << "]" << RESET << " " << options[i] << "\n";
    }
    for (;;) {
        std::string raw = read_line("> ");
        if (is_number(raw)) {
            size_t n = std::stoul(raw);
            if (n >= 1 && n <= options.size()) return n - 1;
        }
        std::cout << RED << "Pick a number from 1 to " << options.size() << "." << RESET << "\n";
    }
}

int ask_int(const std::string& prompt, int def, int lo, int hi) {
    for (;;) {
        std::string raw = read_line(prompt + " [" + std::to_string(def) + "]: ");
        if (raw.empty()) return def;
        if (is_number(raw)) {
            int n = std::stoi(raw);
            if (n >= lo && n <= hi) return n;
        }
        std::cout << RED << "Enter a whole number from " << lo << " to " << hi << "." << RESET << "\n";
    }
}

std::string format_seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", s);
    return buf;
}

double ask_seconds(const std::string& prompt, double def) {
    for (;;) {
        std::string raw = read_line(prompt + " [" + format_seconds(def) + "]: ");
        if (raw.empty()) return def;
        try {
            size_t used = 0;
            double v = std::stod(raw, &used);
            if (used == raw.size() && v >= 0 && v <= 3600) return v;
        } catch (const std::exception&) {
        }
        std::cout << RED << "Enter a number like 0.5 or 2." << RESET << "\n";
    }
}

// ---------------------------------------------------------------------------
// Discord webhook
// ---------------------------------------------------------------------------

struct Config {
    std::string webhook;
    std::string mention;  // "", "@everyone", or a Discord user ID
};

Config load_config() {
    Config cfg;
    std::ifstream f(CONFIG_FILE);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "webhook") cfg.webhook = value;
        else if (key == "mention") cfg.mention = value;
    }
    return cfg;
}

void save_config(const Config& cfg) {
    std::ofstream f(CONFIG_FILE);
    f << "webhook=" << cfg.webhook << "\n" << "mention=" << cfg.mention << "\n";
    if (!f) std::cout << RED << "Couldn't save " << CONFIG_FILE << "." << RESET << "\n";
}

bool looks_like_webhook(const std::string& url) {
    for (const char* host : {"https://discord.com/api/", "https://discordapp.com/api/",
                             "https://ptb.discord.com/api/", "https://canary.discord.com/api/"}) {
        if (starts_with(url, host) && url.find("/webhooks/") != std::string::npos) return true;
    }
    return false;
}

std::string mention_label(const Config& cfg) {
    if (cfg.mention.empty()) return "no ping";
    if (cfg.mention == "@everyone") return "pings @everyone";
    return "pings user " + cfg.mention;
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
    const std::string body = "{\"username\":\"usercheck\",\"content\":" + json_quote(content) +
                             ",\"allowed_mentions\":" + allowed + "}";
    for (int attempt = 0; attempt < 5 && !g_stop; ++attempt) {
        Response r = http("POST", cfg.webhook, body);
        if (r.status >= 200 && r.status < 300) return "";
        if (r.status == 429) {
            nap(retry_after_seconds(r));
            continue;
        }
        if (r.status == 0) return r.error;
        if (r.status == 401 || r.status == 404) return "webhook was deleted or the URL is wrong";
        return http_error(r);
    }
    return "still rate limited after 5 tries";
}

void pick_mention(Config& cfg) {
    size_t who = ask_choice("Who should get pinged when a name is found?",
                            {"Nobody, just post the name", "@everyone", "A specific person (user ID)"});
    if (who == 0) {
        cfg.mention.clear();
    } else if (who == 1) {
        cfg.mention = "@everyone";
    } else {
        std::cout << DIM << "Discord: Settings > Advanced > turn on Developer Mode, then right-click "
                     "the person > Copy User ID." << RESET << "\n";
        for (;;) {
            std::string id = read_line("User ID: ");
            if (id.size() >= 15 && id.size() <= 21 && only_chars(id, DIGITS)) {
                cfg.mention = id;
                break;
            }
            std::cout << RED << "That's not a user ID, it should be 17-20 digits." << RESET << "\n";
        }
    }
}

void webhook_settings(Config& cfg) {
    for (;;) {
        std::string status = cfg.webhook.empty() ? "off" : "on, " + mention_label(cfg);
        size_t pick = ask_choice("Discord webhook (" + status + ")",
                                 {"Set webhook URL", "Change who gets pinged", "Send a test message",
                                  "Turn off", "Back"});
        if (pick == 0) {
            std::cout << DIM << "In Discord: channel settings > Integrations > Webhooks > New Webhook > "
                         "Copy Webhook URL." << RESET << "\n";
            std::string url = read_line("Webhook URL: ");
            if (!looks_like_webhook(url)) {
                std::cout << RED << "That doesn't look like a Discord webhook URL." << RESET << "\n";
                continue;
            }
            Config trial = cfg;
            trial.webhook = url;
            pick_mention(trial);
            std::cout << "Sending a test message...\n";
            std::string err = webhook_send(trial, "usercheck is hooked up. Available usernames will show up here.");
            if (!err.empty()) {
                std::cout << RED << "Didn't work: " << err << RESET << "\n";
                continue;
            }
            cfg = trial;
            save_config(cfg);
            std::cout << GREEN << "Webhook saved. Check your channel for the test message." << RESET << "\n";
        } else if (pick == 1 || pick == 2) {
            if (cfg.webhook.empty()) {
                std::cout << YELLOW << "Set a webhook URL first." << RESET << "\n";
                continue;
            }
            if (pick == 1) {
                pick_mention(cfg);
                save_config(cfg);
            } else {
                std::string err = webhook_send(cfg, "usercheck test message.");
                if (err.empty()) std::cout << GREEN << "Sent." << RESET << "\n";
                else std::cout << RED << "Didn't work: " << err << RESET << "\n";
            }
        } else if (pick == 3) {
            cfg = Config{};
            save_config(cfg);
            std::cout << "Webhook turned off.\n";
        } else {
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Picking the platform and usernames
// ---------------------------------------------------------------------------

Platform custom_platform() {
    std::cout << "\n" << CYAN << "Custom site" << RESET << "\n"
              << "Give a profile URL with {username} where the name goes, for example:\n"
              << "  " << DIM << "https://example.com/users/{username}" << RESET << "\n"
              << "A 404 (page not found) is counted as available, a 200 as taken.\n";
    std::string tmpl;
    for (;;) {
        tmpl = read_line("> URL: ");
        if (tmpl.find("{username}") != std::string::npos &&
            (starts_with(tmpl, "http://") || starts_with(tmpl, "https://"))) {
            break;
        }
        std::cout << RED << "Needs to start with http(s):// and contain {username}." << RESET << "\n";
    }

    std::string host = tmpl.substr(tmpl.find("://") + 3);
    host = host.substr(0, host.find('/'));

    auto check = [tmpl](const std::string& u) {
        std::string url = tmpl;
        size_t pos;
        while ((pos = url.find("{username}")) != std::string::npos) url.replace(pos, 10, url_encode(u));
        return check_profile_url(url);
    };
    auto valid = [](const std::string& u) { return only_chars(u, LOWER + UPPER + DIGITS + "_.-"); };
    return {host.empty() ? "Custom" : host, LOWER + DIGITS, valid, check, 1.0,
            "Plenty of sites return 200 for every URL, or 404 for banned names. "
            "Test it with a name you KNOW exists first."};
}

Platform pick_platform(Config& cfg) {
    for (;;) {
        std::string hook = cfg.webhook.empty() ? "off" : "on";
        size_t pick = ask_choice("Which app do you want to check?",
                                 {"Discord", "Roblox", "Other applications",
                                  "Discord webhook notifications (" + hook + ")"});
        if (pick == 0) return discord_platform();
        if (pick == 1) return roblox_platform();
        if (pick == 3) {
            webhook_settings(cfg);
            continue;
        }
        size_t other = ask_choice("Which other app?", {"Minecraft (Java)", "GitHub", "Custom site (any URL)"});
        if (other == 0) return minecraft_platform();
        if (other == 1) return github_platform();
        return custom_platform();
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

std::vector<std::string> names_from_file(const std::string& path, const Platform& p, bool lowercase) {
    std::ifstream f(path);
    std::vector<std::string> names;
    std::set<std::string> seen;
    std::string line;
    int skipped = 0;
    bool first = true;
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
    if (skipped) {
        std::cout << YELLOW << "Skipped " << skipped << " name(s) that break " << p.name
                  << "'s username rules." << RESET << "\n";
    }
    return names;
}

std::vector<std::string> pick_names(const Platform& p) {
    enum Kind { File, Letters, Chars, Custom };
    struct Mode {
        const char* label;
        Kind kind;
        int length;
    };
    const std::vector<Mode> modes = {
        {"From a .txt file (one name per line)", File, 0},
        {"Random 3 letters      (abc)", Letters, 3},
        {"Random 3 characters   (a1b)", Chars, 3},
        {"Random 4 letters      (abcd)", Letters, 4},
        {"Random 4 characters   (a1b2)", Chars, 4},
        {"Random 5 letters      (abcde)", Letters, 5},
        {"Random 5 characters   (a1b2c)", Chars, 5},
        {"Random custom length", Custom, 0},
    };
    std::vector<std::string> labels;
    for (const Mode& m : modes) labels.push_back(m.label);
    Mode mode = modes[ask_choice("What kind of usernames?", labels)];

    if (mode.kind == File) {
        std::string path;
        for (;;) {
            path = read_line("Path to .txt file (you can drag it in here): ");
            while (!path.empty() && (path.front() == '"' || path.front() == '\'')) path.erase(0, 1);
            while (!path.empty() && (path.back() == '"' || path.back() == '\'')) path.pop_back();
            if (std::ifstream(path)) break;
            std::cout << RED << "Can't find that file." << RESET << "\n";
        }
        // Discord usernames are lowercase only, so lowercase them for the user.
        std::vector<std::string> names = names_from_file(path, p, p.name == "Discord");
        std::cout << "Loaded " << names.size() << " name(s).\n";
        return names;
    }

    if (mode.kind == Custom) {
        mode.length = ask_int("Length", 6, 1, 32);
        mode.kind = ask_choice("Using", {"Letters only", "Letters + numbers + symbols"}) == 0 ? Letters : Chars;
    }

    int count = ask_int("How many to check", 100, 1, 1000000);
    std::vector<std::string> names =
        random_names(mode.kind == Letters ? LOWER : p.charset, mode.length, count, p);
    if (names.empty()) {
        std::cout << RED << p.name << " doesn't allow " << mode.length << "-character names." << RESET << "\n";
    } else if (static_cast<int>(names.size()) < count) {
        std::cout << YELLOW << "Only found " << names.size() << " possible names of that type, checking all of them."
                  << RESET << "\n";
    }
    return names;
}

// ---------------------------------------------------------------------------
// The checking loop
// ---------------------------------------------------------------------------

void save_hit(const std::string& platform, const std::string& name) {
    std::ofstream f(RESULTS_FILE, std::ios::app);
    f << platform << ": " << name << "\n";
}

void run(const Platform& p, const std::vector<std::string>& names, const Config& cfg) {
    double delay = ask_seconds("Seconds between checks (lower = faster but more rate limits)", p.delay);
    std::cout << "\n" << DIM << p.note << RESET << "\n"
              << DIM << "Checking " << names.size() << " name(s) on " << p.name << ". Ctrl+C to stop early."
              << RESET << "\n";
    bool use_webhook = !cfg.webhook.empty();
    if (use_webhook) std::cout << DIM << "Hits get sent to your Discord webhook." << RESET << "\n";
    std::cout << "\n";

    std::vector<std::string> found;
    int taken = 0, invalid = 0, errors = 0;
    g_stop = false;
    g_checking = true;

    for (size_t i = 0; i < names.size() && !g_stop; ++i) {
        const std::string& name = names[i];
        std::string prefix = std::string(DIM) + "[" + std::to_string(i + 1) + "/" +
                             std::to_string(names.size()) + "]" + RESET + " ";
        Result res = p.check(name);
        while (res.state == State::RateLimited && !g_stop) {
            std::cout << prefix << YELLOW << "rate limited, waiting " << static_cast<int>(std::ceil(res.retry_after))
                      << "s..." << RESET << std::endl;
            nap(res.retry_after + 0.5);
            if (!g_stop) res = p.check(name);
        }
        if (g_stop) break;

        switch (res.state) {
            case State::Available: {
                found.push_back(name);
                save_hit(p.name, name);
                std::cout << prefix << GREEN << BOLD << "AVAILABLE: " << name << RESET;
                if (!res.detail.empty()) std::cout << " " << DIM << "(" << res.detail << ")" << RESET;
                std::cout << "\a" << std::endl;
                if (use_webhook) {
                    std::string err = webhook_send(cfg, "\xE2\x9C\x85 `" + name + "` is available on **" + p.name + "**");
                    if (!err.empty()) {
                        std::cout << prefix << YELLOW << "webhook failed (" << err
                                  << "), turning it off for this run" << RESET << std::endl;
                        use_webhook = false;
                    }
                }
                break;
            }
            case State::Taken:
                ++taken;
                std::cout << prefix << RED << "taken" << RESET << "     " << name << std::endl;
                break;
            case State::Invalid:
                ++invalid;
                std::cout << prefix << YELLOW << "not allowed" << RESET << " " << name << " " << DIM << "("
                          << res.detail << ")" << RESET << std::endl;
                break;
            default:
                ++errors;
                std::cout << prefix << YELLOW << "error" << RESET << "     " << name << " " << DIM << "("
                          << res.detail << ")" << RESET << std::endl;
        }
        if (i + 1 < names.size()) nap(delay);
    }

    g_checking = false;
    if (g_stop) std::cout << "\n" << YELLOW << "Stopped." << RESET << "\n";
    g_stop = false;

    std::cout << "\n" << BOLD << "Done." << RESET << " " << GREEN << found.size() << " available" << RESET << ", "
              << taken << " taken, " << invalid << " not allowed, " << errors << " errors.\n";
    if (!found.empty()) {
        std::cout << GREEN << "Available: ";
        for (size_t i = 0; i < found.size(); ++i) std::cout << (i ? ", " : "") << found[i];
        std::cout << RESET << "\n" << DIM << "Saved to " << RESULTS_FILE << RESET << "\n";
    }
}

}  // namespace

int main() {
    setup_console();
    std::signal(SIGINT, on_sigint);
    Config cfg = load_config();

    std::cout << BOLD << CYAN << "usercheck" << RESET << " " << DIM << "find unclaimed usernames" << RESET << "\n";
    for (;;) {
        Platform p = pick_platform(cfg);
        std::vector<std::string> names = pick_names(p);
        if (!names.empty()) run(p, names, cfg);
        if (ask_choice("Again?", {"Yes", "No, quit"}) == 1) break;
    }
    return 0;
}
