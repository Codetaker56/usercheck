#!/usr/bin/env python3
"""Runs ringer against tests/fake_server.py and checks what it does.

    python3 tests/run_tests.py           # all of them
    python3 tests/run_tests.py github    # just the ones with "github" in the name

It builds build/ringer-test first, a copy of ringer that sends every request to the fake server.
Needs g++, libcurl's headers and graphql-core (pip install -r tests/requirements.txt). Linux and
macOS only, since a few tests press Ctrl+C with a signal.

Each test types into ringer's menus exactly like a person would, in a fresh folder, and then looks
at the screen, the files ringer wrote, and the requests the fake server got. A few wait out real
rate limit timers (Lichess asks for a whole minute), so the full run takes about 3 minutes.
"""
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "build", "ringer-test")
FAKE = os.path.join(ROOT, "tests", "fake_server.py")
PORT = 8765
TOKEN = "ghp_good123"
WEBHOOK = "https://discord.com/api/webhooks/1/abc"

# Main menu: 1 Discord, 2 Roblox, 3 Other apps, 4 Every app, 5 Webhook pings.
# Other apps: 1 Minecraft, 2 GitHub, 3 Lichess, 4 Chess.com, 5 GitLab, 6 Custom site, 7 GitHub token.
DISCORD, ROBLOX, MINECRAFT, GITHUB, LICHESS, CHESSCOM, GITLAB = "1", "2", "3\n1", "3\n2", "3\n3", "3\n4", "3\n5"


def from_file(app, filename):
    """Keys for: pick `app`, check names from `filename` with no delay, then quit."""
    return f"{app}\n1\n{filename}\n0\n0\n"


def build():
    os.makedirs(os.path.dirname(BINARY), exist_ok=True)
    subprocess.run(["g++", "-std=c++17", "-O1", "-Wall", "-Wextra", f'-DTEST_SERVER="http://127.0.0.1:{PORT}"',
                    os.path.join(ROOT, "ringer.cpp"), "-o", BINARY, "-lcurl"], check=True)


class Run:
    """One run of ringer: its screen, the files it left behind and the requests the fake got."""

    def __init__(self, keys, scenario="", files=None, cfg="", timeout=60, interrupt_after=None):
        self.dir = tempfile.mkdtemp(prefix="ringer-test-")
        self.screen = ""
        RUNS.append(self)
        for name, text in (files or {}).items():
            self.write(name, text)
        if cfg:
            self.write("ringer.cfg", cfg)
        log = os.path.join(self.dir, "fake.log")
        env = dict(os.environ, SCENARIO=scenario, FAKE_LOG=log)
        server = subprocess.Popen([sys.executable, FAKE, str(PORT)], env=env, stderr=subprocess.PIPE)
        try:
            wait_for_port(server)
            ringer = subprocess.Popen([BINARY], cwd=self.dir, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
            if interrupt_after is None:
                out, _ = ringer.communicate(keys.encode(), timeout=timeout)
            else:
                # Keep stdin open so ringer doesn't quit before the Ctrl+C, then press 0 to leave.
                ringer.stdin.write(keys.encode())
                ringer.stdin.flush()
                time.sleep(interrupt_after)
                ringer.send_signal(signal.SIGINT)
                time.sleep(1)
                out, _ = ringer.communicate(b"0\n", timeout=timeout)
        finally:
            server.kill()
            server.wait()
        # Colors out, and every redraw of a line (\r) on a line of its own.
        self.screen = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", out.decode(errors="replace")).replace("\r", "\n")
        self.requests = []
        self.graphql_errors = []
        if os.path.exists(log):
            for line in open(log):
                entry = json.loads(line)
                (self.graphql_errors if "graphql_errors" in entry else self.requests).append(entry)

    def write(self, name, text):
        with open(os.path.join(self.dir, name), "w") as f:
            f.write(text)

    def file(self, name):
        path = os.path.join(self.dir, name)
        return open(path).read() if os.path.exists(path) else ""

    def to(self, host, path_part=""):
        return [r for r in self.requests if r["host"] == host and path_part in r["path"]]

    def summary(self):
        """The last summary card, squashed to single spaces."""
        start = max(self.screen.rfind("+- Done"), self.screen.rfind("+- Stopped early"))
        assert start >= 0, "no summary card on screen"
        return re.sub(r"\s+", " ", self.screen[start:self.screen.find("What next?", start)])

    def expect(self, *texts):
        for text in texts:
            assert text in self.screen, f"{text!r} not on screen"

    def expect_summary(self, *texts):
        card = self.summary()
        for text in texts:
            assert text in card, f"{text!r} not in summary: {card}"

    def clean_up(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def wait_for_port(server):
    for _ in range(100):
        if server.poll() is not None:
            raise RuntimeError("fake server died: " + server.stderr.read().decode())
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("fake server never started")


TESTS = []
RUNS = []  # every Run a test made, so they can be shown if it fails and cleaned up after


def test(fn):
    TESTS.append(fn)
    return fn


# --- main menu --------------------------------------------------------------------------------

@test
def main_menu_lists_everything():
    r = Run("0\n")
    r.expect("[1]  Discord", "[2]  Roblox", "[3]  Other apps", "[4]  Every app", "[5]  Webhook pings", "[0]  Quit")


# --- Discord ----------------------------------------------------------------------------------

@test
def discord_double_checks_names_that_look_free():
    r = Run(from_file(DISCORD, "names.txt"), files={"names.txt": "discord\nfreedc\n"})
    r.expect_summary("1 available", "1 taken")
    assert len(r.to("discord.com", "suggestions")) == 2
    assert len(r.to("discord.com", "attempt")) == 1, "only the free-looking name gets double-checked"
    assert r.file("available.txt") == "Discord: freedc\n"


@test
def discord_hits_are_unconfirmed_while_the_double_check_rests():
    r = Run(from_file(DISCORD, "names.txt"), scenario="discord_attempt_limited",
            files={"names.txt": "freeone\nfreetwo\n"})
    r.expect_summary("2 available", "? = not double-checked, 2 of them")
    assert len(r.to("discord.com", "attempt")) == 1, "a resting double-check isn't asked again"
    assert "Discord: freetwo (not double-checked)" in r.file("available.txt")


# --- Roblox -----------------------------------------------------------------------------------

@test
def roblox_looks_up_100_at_a_time_and_falls_back_when_a_batch_fails():
    names = ["Roblox", "takenRB"] + [f"free{i}" for i in range(1, 99)] + ["builderman", "badword"] + \
            [f"free{i}" for i in range(101, 149)]
    r = Run(from_file(ROBLOX, "names.txt"), scenario="roblox_batch_fails", files={"names.txt": "\n".join(names)})
    r.expect("Up to 100 names per request.", "Batch lookup failed (HTTP 500", "Username not appropriate for Roblox")
    r.expect_summary("146 available", "3 taken", "1 not allowed", "0 errors")
    assert [len(json.loads(q["body"])["usernames"]) for q in r.to("users.roblox.com")] == [100, 50]
    # 98 misses from the first batch get confirmed, and all 50 of the failed one get checked.
    assert len(r.to("auth.roblox.com")) == 148


@test
def roblox_matches_names_whatever_the_case():
    r = Run(from_file(ROBLOX, "names.txt"), files={"names.txt": "Roblox\nROBLOX\nfreerb\n"})
    r.expect_summary("1 available", "2 taken")
    assert len(r.to("users.roblox.com")) == 1 and len(r.to("auth.roblox.com")) == 1


# --- Minecraft --------------------------------------------------------------------------------

@test
def minecraft_looks_up_10_at_a_time():
    names = ["Notch", "NOTCH", "jeb_", "freemc"] + [f"free{i}" for i in range(1, 24)]
    r = Run(from_file(MINECRAFT, "names.txt"), files={"names.txt": "\n".join(names)})
    r.expect_summary("24 available", "3 taken")
    assert [len(json.loads(q["body"])) for q in r.to("api.minecraftservices.com")] == [10, 10, 7]
    assert not r.to("api.mojang.com"), "no one-name lookups when the batches work"


# --- Lichess ----------------------------------------------------------------------------------

@test
def lichess_counts_closed_accounts_and_waits_a_minute_after_a_429():
    r = Run(from_file(LICHESS, "names.txt"), scenario="lichess_429", timeout=120,
            files={"names.txt": "DrNykterstein\nclosedone\nfreeli\n1bad\n"})
    r.expect("Skipped 1 name(s) that break Lichess's username rules", "rate limited: 1m")
    r.expect_summary("1 available", "2 taken", "Slowed by 1 rate limit")
    batches = r.to("lichess.org", "/api/users")
    assert len(batches) == 2 and batches[0]["body"] == batches[1]["body"] == "DrNykterstein,closedone,freeli"
    assert batches[0]["ctype"] == "text/plain"
    assert batches[1]["t"] - batches[0]["t"] >= 59


@test
def ctrl_c_during_a_wait_saves_the_unchecked_names():
    names = "drnykterstein\n" + "".join(f"freeli{i}\n" for i in range(1, 10))
    r = Run(f"{LICHESS}\n1\nnames.txt\n0\n", scenario="lichess_429", interrupt_after=4,
            files={"names.txt": names})
    r.expect_summary("Stopped early", "10 names saved to unchecked.txt")
    assert r.file("unchecked.txt") == names


# --- GitHub -----------------------------------------------------------------------------------

@test
def github_token_is_checked_before_it_is_saved():
    r = Run("3\n7\n1\nghp_bad\n1\nnot a token\n" + TOKEN + "\n0\n0\n0\n")
    r.expect("Didn't work: GitHub says that token isn't valid", "That doesn't look like a GitHub token.",
             "Token saved. GitHub checks use the API now.")
    assert "github_token=" + TOKEN in r.file("ringer.cfg")
    assert [q["auth"] for q in r.to("api.github.com", "/rate_limit")] == ["Bearer ghp_bad", "Bearer " + TOKEN]


@test
def github_without_a_token_uses_profile_pages():
    r = Run(from_file(GITHUB, "names.txt"), files={"names.txt": "torvalds\nanthropics\nfreegh\n"})
    r.expect_summary("1 available", "2 taken")
    assert len(r.to("github.com")) == 3 and not r.to("api.github.com")


@test
def github_with_a_token_looks_up_100_at_a_time():
    names = [f"freegh{i}" for i in range(1, 98)] + ["torvalds", "TakenGH", "anthropics"] + \
            [f"freegh{i}" for i in range(98, 148)]
    r = Run(from_file(GITHUB, "names.txt"), cfg=f"github_token={TOKEN}\n", files={"names.txt": "\n".join(names)})
    r.expect_summary("147 available", "3 taken")
    queries = [json.loads(q["body"])["query"] for q in r.to("api.github.com", "/graphql")]
    assert [q.count("repositoryOwner") for q in queries] == [100, 50]
    assert not r.graphql_errors, r.graphql_errors
    assert not r.to("api.github.com", "/users/"), "no one-name lookups when the batches work"


@test
def github_graphql_rate_limit_is_waited_out():
    r = Run(from_file(GITHUB, "names.txt"), scenario="gh_graphql_ratelimited", cfg=f"github_token={TOKEN}\n",
            files={"names.txt": "torvalds\nfreeone\nanthropics\nfreetwo\ntakengh\n"})
    r.expect_summary("2 available", "3 taken", "Slowed by 1 rate limit")
    assert len(r.to("api.github.com", "/graphql")) == 2


@test
def github_graphql_failure_falls_back_to_one_at_a_time():
    r = Run(from_file(GITHUB, "names.txt"), scenario="gh_graphql_broken", cfg=f"github_token={TOKEN}\n",
            files={"names.txt": "torvalds\ntakenGH\nfreegh\nprimarylimit\nsecondarylimit\n"})
    r.expect("Batch lookup failed (HTTP 502")
    # The REST check honors both kinds of GitHub limit: a reset time and Retry-After.
    r.expect_summary("3 available", "2 taken", "Slowed by 2 rate limits")
    assert len(r.to("api.github.com", "/users/")) == 7


@test
def github_answer_missing_a_name_falls_back():
    r = Run(from_file(GITHUB, "names.txt"), scenario="gh_graphql_drops", cfg=f"github_token={TOKEN}\n",
            files={"names.txt": "torvalds\nfreeone\nanthropics\nfreetwo\ntakengh\n"})
    r.expect("GitHub's answer left some names out")
    r.expect_summary("2 available", "3 taken")
    assert len(r.to("api.github.com", "/users/")) == 5


@test
def github_errors_before_data_are_fine():
    r = Run(from_file(GITHUB, "names.txt"), scenario="gh_errors_first", cfg=f"github_token={TOKEN}\n",
            files={"names.txt": "torvalds\nfreeone\nanthropics\nfreetwo\ntakengh\n"})
    assert "Batch lookup failed" not in r.screen
    r.expect_summary("2 available", "3 taken")


@test
def github_bio_saying_rate_limited_is_not_a_rate_limit():
    r = Run(from_file(GITHUB, "names.txt"), scenario="gh_graphql_broken", cfg=f"github_token={TOKEN}\n",
            files={"names.txt": "ratelimitbio\nfreeone\n"}, timeout=20)
    r.expect_summary("1 available", "1 taken")
    assert "Slowed by" not in r.summary() and len(r.to("api.github.com", "/users/")) == 2


# --- Chess.com --------------------------------------------------------------------------------

@test
def chesscom_double_checks_hits_until_cloudflare_says_stop():
    names = "hikaru\nHIKARU\nfreeonecc\nbadwordcc\nfreetwocc\nfreethreecc\ntakencc\n"
    r = Run(from_file(CHESSCOM, "names.txt"), scenario="cc_challenge", files={"names.txt": names})
    r.expect("That username contains a word that is not all")
    r.expect_summary("3 available", "3 taken", "1 not allowed", "? = not double-checked, 2 of them")
    # Only names with no account get the sign-up check, and not once it's resting.
    assert len(r.to("www.chess.com")) == 3
    assert r.file("available.txt") == ("Chess.com: freeonecc\nChess.com: freetwocc (not double-checked)\n"
                                       "Chess.com: freethreecc (not double-checked)\n")


# --- GitLab -----------------------------------------------------------------------------------

@test
def gitlab_reserved_names_and_429s():
    r = Run(from_file(GITLAB, "names.txt"), scenario="gitlab_429",
            files={"names.txt": "sytses\ngitlab-org\napi\nfreegl\nexplore\n-abc\n"})
    r.expect("reserved by GitLab", "rate limited: 15s", "Skipped 1 name(s)")
    r.expect_summary("1 available", "2 taken", "2 not allowed", "Slowed by 1 rate limit")
    assert not r.to("gitlab.com", "sign_in"), "redirects aren't followed"


# --- Settings ---------------------------------------------------------------------------------

@test
def turning_off_the_webhook_keeps_the_rest_of_the_settings():
    later = int(time.time()) + 3000
    r = Run("5\n4\n0\n0\n", cfg=f"webhook={WEBHOOK}\nmention=\ngithub_token={TOKEN}\nlimited_until.Discord={later}\n")
    r.expect("Webhook turned off.")
    cfg = r.file("ringer.cfg")
    assert "webhook=\n" in cfg and f"github_token={TOKEN}" in cfg and f"limited_until.Discord={later}" in cfg, cfg


# --- Every app --------------------------------------------------------------------------------

@test
def every_app_checks_each_name_everywhere():
    r = Run("4\nfreeall torvalds a.b @freeall\n0\n", cfg=f"webhook={WEBHOOK}\nmention=\n")
    r.expect("Checking 3 names on 7 apps.", "breaks Roblox's username rules")
    r.expect_summary("freeall free everywhere", "torvalds free on Roblox, Minecraft (Java), Lichess, Chess.com, GitLab",
                     "a.b free on Discord, GitLab", "3 pings sent")
    pings = [json.loads(q["body"])["content"] for q in r.to("discord.com", "/api/webhooks/")]
    assert pings[2] == "✅ `a.b` is available on **Discord**, **GitLab**", pings
    assert len(r.file("available.txt").splitlines()) == 14


@test
def every_app_skips_an_app_that_wants_a_long_wait():
    r = Run("4\nfreeone freetwo\n0\n", scenario="discord_limited")
    r.expect("rate limited  (skipping it until", "skipped       (rate limited until")
    r.expect_summary("Skipped Discord from freeone on")
    assert "limited_until.Discord=" in r.file("ringer.cfg")
    assert len(r.to("discord.com")) == 2, "Discord isn't asked again once it's skipped"


@test
def every_app_waits_out_short_limits_before_skipping():
    r = Run("4\nfreeone freetwo\n0\n", scenario="gitlab_always_429", timeout=90)
    r.expect("rate limited, waiting 15s", "skipped       (rate limited until")
    r.expect_summary("Skipped GitLab from freeone on")
    assert len(r.to("gitlab.com")) == 3, "two waits, then it gives up"


@test
def every_app_ctrl_c_marks_the_name_it_stopped_on():
    r = Run("4\nfreeone freetwo\n", scenario="gitlab_always_429", interrupt_after=5)
    r.expect_summary("Stopped early", "freeone free on", "(stopped partway)")
    assert "freetwo" not in r.summary()


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else ""
    build()
    failed = []
    for fn in TESTS:
        if only not in fn.__name__:
            continue
        start = time.time()
        try:
            fn()
            print(f"ok    {fn.__name__} ({time.time() - start:.0f}s)", flush=True)
        except Exception:
            failed.append(fn.__name__)
            print(f"FAIL  {fn.__name__}", flush=True)
            traceback.print_exc()
            if RUNS:
                print("---- last 40 lines ringer showed ----\n" + "\n".join(RUNS[-1].screen.splitlines()[-40:]))
        finally:
            for run in RUNS:
                run.clean_up()
            RUNS.clear()
    print(f"\n{len(failed)} failed" if failed else "\nall passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
