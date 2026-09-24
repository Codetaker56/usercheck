#!/usr/bin/env python3
"""usercheck: find unclaimed usernames on Discord, Roblox and a few other sites.

Standard library only, so it runs on any Python 3.8+ with nothing to install.
"""

import json
import os
import random
import re
import string
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0 Safari/537.36"
)
RESULTS_FILE = "available.txt"

# Result states
AVAILABLE = "available"
TAKEN = "taken"
INVALID = "invalid"
ERROR = "error"


# --------------------------------------------------------------------------
# Terminal colours
# --------------------------------------------------------------------------

if os.name == "nt":
    os.system("")  # turns on ANSI escape codes in the Windows console

GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
CYAN = "\033[96m"
DIM = "\033[2m"
BOLD = "\033[1m"
RESET = "\033[0m"


# --------------------------------------------------------------------------
# HTTP helper
# --------------------------------------------------------------------------

class RateLimited(Exception):
    def __init__(self, retry_after):
        super().__init__(f"rate limited for {retry_after:.1f}s")
        self.retry_after = retry_after


def http(method, url, body=None, timeout=15):
    """Returns (status_code, body_text). Never raises on HTTP error codes."""
    data = None
    headers = {"User-Agent": USER_AGENT, "Accept": "application/json, text/html"}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        text = e.read().decode("utf-8", "replace")
        if e.code == 429:
            retry = e.headers.get("Retry-After")
            try:
                retry = float(json.loads(text).get("retry_after", retry))
            except (ValueError, AttributeError, TypeError):
                pass
            try:
                retry = float(retry)
            except (TypeError, ValueError):
                retry = 10.0
            raise RateLimited(max(retry, 1.0))
        return e.code, text


# --------------------------------------------------------------------------
# Platforms
# --------------------------------------------------------------------------
# Each platform has:
#   name       shown in the menu
#   charset    characters used for random "characters" usernames
#   valid(u)   local rule check so we don't waste requests on impossible names
#   check(u)   returns (state, detail)
#   delay      default seconds between requests
#   note       caveat shown before checking


def check_discord(u):
    status, text = http(
        "POST",
        "https://discord.com/api/v9/unique-username/username-attempt-unauthed",
        {"username": u},
    )
    try:
        data = json.loads(text)
    except ValueError:
        return ERROR, f"HTTP {status}"
    if status == 200 and "taken" in data:
        return (TAKEN, "") if data["taken"] else (AVAILABLE, "")
    if data.get("code") == 50035:
        try:
            msg = data["errors"]["username"]["_errors"][0]["message"]
        except (KeyError, IndexError, TypeError):
            msg = "invalid"
        return INVALID, msg
    return ERROR, f"HTTP {status}: {text[:80]}"


def valid_discord(u):
    return (
        2 <= len(u) <= 32
        and re.fullmatch(r"[a-z0-9_.]+", u) is not None
        and ".." not in u
    )


def check_roblox(u):
    query = urllib.parse.urlencode(
        {"Username": u, "Birthday": "2000-01-01T00:00:00.000Z"}
    )
    status, text = http(
        "GET", f"https://auth.roblox.com/v1/usernames/validate?{query}"
    )
    try:
        data = json.loads(text)
    except ValueError:
        return ERROR, f"HTTP {status}"
    code = data.get("code")
    if code == 0:
        return AVAILABLE, ""
    if code == 1:
        return TAKEN, ""
    if code is not None:
        return INVALID, data.get("message", f"code {code}")
    return ERROR, f"HTTP {status}: {text[:80]}"


def valid_roblox(u):
    return (
        3 <= len(u) <= 20
        and re.fullmatch(r"[A-Za-z0-9_]+", u) is not None
        and u.count("_") <= 1
        and not u.startswith("_")
        and not u.endswith("_")
    )


def check_minecraft(u):
    status, text = http(
        "GET", f"https://api.mojang.com/users/profiles/minecraft/{urllib.parse.quote(u)}"
    )
    if status == 200:
        return TAKEN, ""
    if status in (204, 404):
        return AVAILABLE, "no profile found"
    return ERROR, f"HTTP {status}"


def valid_minecraft(u):
    return 3 <= len(u) <= 16 and re.fullmatch(r"[A-Za-z0-9_]+", u) is not None


def check_github(u):
    status, _ = http("GET", f"https://github.com/{urllib.parse.quote(u)}")
    if status == 200:
        return TAKEN, ""
    if status == 404:
        return AVAILABLE, "no profile found"
    return ERROR, f"HTTP {status}"


def valid_github(u):
    return (
        1 <= len(u) <= 39
        and re.fullmatch(r"[A-Za-z0-9-]+", u) is not None
        and not u.startswith("-")
        and not u.endswith("-")
        and "--" not in u
    )


def make_custom_platform():
    print(f"\n{CYAN}Custom site{RESET}")
    print("Give a profile URL with {username} where the name goes, for example:")
    print(f"  {DIM}https://example.com/users/{{username}}{RESET}")
    print("A 404 (page not found) is counted as available, a 200 as taken.")
    while True:
        template = input("> URL: ").strip()
        if "{username}" in template and template.startswith(("http://", "https://")):
            break
        print(f"{RED}Needs to start with http(s):// and contain {{username}}.{RESET}")

    def check(u):
        status, _ = http("GET", template.replace("{username}", urllib.parse.quote(u)))
        if status == 404:
            return AVAILABLE, "404"
        if 200 <= status < 300:
            return TAKEN, ""
        return ERROR, f"HTTP {status}"

    return {
        "name": urllib.parse.urlparse(template).netloc or "Custom",
        "charset": string.ascii_lowercase + string.digits,
        "valid": lambda u: re.fullmatch(r"[A-Za-z0-9_.\-]+", u) is not None,
        "check": check,
        "delay": 1.0,
        "note": "Plenty of sites return 200 for every URL, or 404 for banned names. "
                "Test it with a name you KNOW exists first.",
    }


PLATFORMS = {
    "discord": {
        "name": "Discord",
        "charset": string.ascii_lowercase + string.digits + "_.",
        "valid": valid_discord,
        "check": check_discord,
        "delay": 1.5,
        "note": "Uses the same public check as Discord's sign-up page. "
                "Discord rate limits this hard, the script waits it out automatically.",
    },
    "roblox": {
        "name": "Roblox",
        "charset": string.ascii_lowercase + string.digits + "_",
        "valid": valid_roblox,
        "check": check_roblox,
        "delay": 0.5,
        "note": "Uses Roblox's sign-up validation, so 'available' means Roblox "
                "would actually let you register it.",
    },
    "minecraft": {
        "name": "Minecraft (Java)",
        "charset": string.ascii_lowercase + string.digits + "_",
        "valid": valid_minecraft,
        "check": check_minecraft,
        "delay": 1.0,
        "note": "Checks for an existing profile. Names that were just changed are "
                "locked for a while and banned names also show as free.",
    },
    "github": {
        "name": "GitHub",
        "charset": string.ascii_lowercase + string.digits + "-",
        "valid": valid_github,
        "check": check_github,
        "delay": 1.5,
        "note": "Checks whether the profile page exists. Reserved and deleted "
                "names can 404 but still can't be registered.",
    },
}


# --------------------------------------------------------------------------
# Username sources
# --------------------------------------------------------------------------

def random_names(charset, length, count, platform):
    """Yields up to `count` unique random names valid on the platform."""
    seen = set()
    space = len(charset) ** length
    attempts = 0
    while len(seen) < count and attempts < count * 50 and len(seen) < space:
        attempts += 1
        name = "".join(random.choice(charset) for _ in range(length))
        if name in seen or not platform["valid"](name):
            continue
        seen.add(name)
        yield name


def names_from_file(path, platform):
    with open(path, encoding="utf-8", errors="replace") as f:
        raw = [line.strip() for line in f]
    names, seen, skipped = [], set(), 0
    for name in raw:
        if not name or name.startswith("#"):
            continue
        # Discord usernames are lowercase only, lowercase them for the user.
        if platform is PLATFORMS["discord"]:
            name = name.lower()
        if name in seen:
            continue
        seen.add(name)
        if not platform["valid"](name):
            skipped += 1
            continue
        names.append(name)
    if skipped:
        print(f"{YELLOW}Skipped {skipped} name(s) that break {platform['name']}'s "
              f"username rules.{RESET}")
    return names


# --------------------------------------------------------------------------
# Menus
# --------------------------------------------------------------------------

def ask_choice(title, options):
    """options is a list of labels. Returns the 0-based index picked."""
    print(f"\n{BOLD}{title}{RESET}")
    for i, label in enumerate(options, 1):
        print(f"  {CYAN}[{i}]{RESET} {label}")
    while True:
        raw = input("> ").strip()
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return int(raw) - 1
        print(f"{RED}Pick a number from 1 to {len(options)}.{RESET}")


def ask_int(prompt, default, lo=1, hi=1_000_000):
    while True:
        raw = input(f"{prompt} [{default}]: ").strip()
        if not raw:
            return default
        if raw.isdigit() and lo <= int(raw) <= hi:
            return int(raw)
        print(f"{RED}Enter a whole number from {lo} to {hi}.{RESET}")


def ask_float(prompt, default):
    while True:
        raw = input(f"{prompt} [{default}]: ").strip()
        if not raw:
            return default
        try:
            value = float(raw)
            if value >= 0:
                return value
        except ValueError:
            pass
        print(f"{RED}Enter a number like 0.5 or 2.{RESET}")


def pick_platform():
    keys = list(PLATFORMS)
    idx = ask_choice("Which app do you want to check?",
                     ["Discord", "Roblox", "Other applications"])
    if idx == 0:
        return PLATFORMS["discord"]
    if idx == 1:
        return PLATFORMS["roblox"]
    others = [k for k in keys if k not in ("discord", "roblox")]
    idx = ask_choice("Which other app?",
                     [PLATFORMS[k]["name"] for k in others] + ["Custom site (any URL)"])
    if idx == len(others):
        return make_custom_platform()
    return PLATFORMS[others[idx]]


MODES = [
    ("From a .txt file (one name per line)", None, None),
    ("Random 3 letters      (abc)", "letters", 3),
    ("Random 3 characters   (a1b)", "chars", 3),
    ("Random 4 letters      (abcd)", "letters", 4),
    ("Random 4 characters   (a1b2)", "chars", 4),
    ("Random 5 letters      (abcde)", "letters", 5),
    ("Random 5 characters   (a1b2c)", "chars", 5),
    ("Random custom length", "custom", None),
]


def pick_names(platform):
    idx = ask_choice("What kind of usernames?", [m[0] for m in MODES])
    _, kind, length = MODES[idx]

    if kind is None:
        while True:
            path = input("Path to .txt file (you can drag it in here): ").strip().strip('"\'')
            if os.path.isfile(path):
                break
            print(f"{RED}Can't find that file.{RESET}")
        names = names_from_file(path, platform)
        print(f"Loaded {len(names)} name(s).")
        return names

    if kind == "custom":
        length = ask_int("Length", 6, 1, 32)
        kind = "chars" if ask_choice("Using", ["Letters only", "Letters + numbers + symbols"]) else "letters"

    if kind == "letters":
        charset = string.ascii_lowercase
    else:
        charset = platform["charset"]

    count = ask_int("How many to check", 100)
    names = list(random_names(charset, length, count, platform))
    if not names:
        print(f"{RED}{platform['name']} doesn't allow {length}-character names.{RESET}")
    elif len(names) < count:
        print(f"{YELLOW}Only {len(names)} possible names of that type, checking all of them.{RESET}")
    return names


# --------------------------------------------------------------------------
# Checking loop
# --------------------------------------------------------------------------

def save_hit(platform_name, name):
    with open(RESULTS_FILE, "a", encoding="utf-8") as f:
        f.write(f"{platform_name}: {name}\n")


def run(platform, names):
    delay = ask_float("Seconds between checks (lower = faster but more rate limits)",
                      platform["delay"])
    print(f"\n{DIM}{platform['note']}{RESET}")
    print(f"{DIM}Checking {len(names)} name(s) on {platform['name']}. "
          f"Ctrl+C to stop early.{RESET}\n")

    found, taken, invalid, errors = [], 0, 0, 0
    try:
        for i, name in enumerate(names, 1):
            prefix = f"{DIM}[{i}/{len(names)}]{RESET}"
            while True:
                try:
                    state, detail = platform["check"](name)
                    break
                except RateLimited as e:
                    print(f"{prefix} {YELLOW}rate limited, waiting "
                          f"{e.retry_after:.0f}s...{RESET}")
                    time.sleep(e.retry_after + 0.5)
                except (urllib.error.URLError, TimeoutError, OSError) as e:
                    state, detail = ERROR, str(getattr(e, "reason", e))
                    break

            if state == AVAILABLE:
                found.append(name)
                save_hit(platform["name"], name)
                extra = f" {DIM}({detail}){RESET}" if detail else ""
                print(f"{prefix} {GREEN}{BOLD}AVAILABLE: {name}{RESET}{extra}\a")
            elif state == TAKEN:
                taken += 1
                print(f"{prefix} {RED}taken{RESET}     {name}")
            elif state == INVALID:
                invalid += 1
                print(f"{prefix} {YELLOW}not allowed{RESET} {name} {DIM}({detail}){RESET}")
            else:
                errors += 1
                print(f"{prefix} {YELLOW}error{RESET}     {name} {DIM}({detail}){RESET}")

            if i < len(names):
                time.sleep(delay)
    except KeyboardInterrupt:
        print(f"\n{YELLOW}Stopped.{RESET}")

    print(f"\n{BOLD}Done.{RESET} {GREEN}{len(found)} available{RESET}, "
          f"{taken} taken, {invalid} not allowed, {errors} errors.")
    if found:
        print(f"{GREEN}Available: {', '.join(found)}{RESET}")
        print(f"{DIM}Saved to {os.path.abspath(RESULTS_FILE)}{RESET}")


def main():
    print(f"{BOLD}{CYAN}usercheck{RESET} {DIM}find unclaimed usernames{RESET}")
    while True:
        platform = pick_platform()
        names = pick_names(platform)
        if names:
            run(platform, names)
        if ask_choice("Again?", ["Yes", "No, quit"]) == 1:
            break


if __name__ == "__main__":
    try:
        main()
    except (KeyboardInterrupt, EOFError):
        print()
        sys.exit(0)
