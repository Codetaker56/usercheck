#!/usr/bin/env python3
"""A fake version of every site ringer checks, for tests/run_tests.py.

ringer built with -DTEST_SERVER='"http://127.0.0.1:8765"' sends every request here, with the real
host moved into the path: https://discord.com/api/v9/... arrives as /discord.com/api/v9/...

Every request gets logged as one line of JSON to $FAKE_LOG, so the tests can check what ringer sent.
$SCENARIO is a comma separated list of the switches below that make a site misbehave.

    python3 tests/fake_server.py 8765
"""
import base64
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlsplit

from graphql import build_schema, graphql_sync

# The part of GitHub's GraphQL schema ringer uses, so its queries get parsed and validated for real.
GH_SCHEMA = build_schema("""
interface RepositoryOwner { login: String! }
type User implements RepositoryOwner { login: String! }
type Organization implements RepositoryOwner { login: String! }
type Query { repositoryOwner(login: String!): RepositoryOwner }
""")

SCENARIO = set(os.environ.get("SCENARIO", "").split(","))
LOG = open(os.environ.get("FAKE_LOG", "fake.log"), "a")
counts = {}
# For mc_available_bucket: Minecraft's logged in check allowing 5 at once, then 2 a second.
bucket = {"tokens": 5.0, "at": time.time()}


def take_token():
    now = time.time()
    bucket["tokens"] = min(5.0, bucket["tokens"] + (now - bucket["at"]) * 2)
    bucket["at"] = now
    if bucket["tokens"] < 1:
        return False
    bucket["tokens"] -= 1
    return True

# Who has which name on each fake site, all lowercase.
DISCORD_TAKEN = {"discord", "torvalds", "takendc"}
ROBLOX_TAKEN = {"roblox", "builderman", "takenrb"}
ROBLOX_BAD = {"badword"}  # Roblox's filter turns these down
MC_TAKEN = {"notch": "Notch", "jeb_": "jeb_"}
MC_HELD = {"1kd", "_an"}  # nobody has them, but Minecraft is holding them
MC_NOT_ALLOWED = {"badmc"}
LICHESS_TAKEN = {"drnykterstein", "closedone"}  # closedone is a closed account
GH_USERS = {"torvalds", "takengh"}
GH_ORGS = {"anthropics"}
GH_TOKEN = "ghp_good123"  # the only token the fake GitHub accepts
CC_TAKEN = {"hikaru", "takencc"}
CC_BANNED = {"badwordcc"}  # Chess.com's banned word list
GL_TAKEN = {"sytses", "gitlab-org"}
GL_RESERVED = {"api", "explore"}  # redirect to the sign-in page, like the real ones


def bump(key):
    counts[key] = counts.get(key, 0) + 1
    return counts[key]


class Fake(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, body="", headers=None, ctype="application/json"):
        data = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.answer("GET")

    def do_POST(self):
        self.answer("POST")

    def answer(self, method):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        url = urlsplit(self.path)
        host, _, rest = url.path.lstrip("/").partition("/")
        path = "/" + rest
        q = parse_qs(url.query)
        LOG.write(json.dumps({"t": time.time(), "method": method, "host": host, "path": path, "query": url.query,
                              "body": body.decode(errors="replace"), "auth": self.headers.get("Authorization"),
                              "ctype": self.headers.get("Content-Type")}) + "\n")
        LOG.flush()
        handler = {
            "discord.com": self.discord,
            "users.roblox.com": self.roblox_batch,
            "auth.roblox.com": self.roblox_validate,
            "api.minecraftservices.com": self.minecraft_services,
            "api.mojang.com": self.minecraft_one,
            "lichess.org": self.lichess,
            "api.github.com": self.github_api,
            "github.com": self.github_page,
            "api.chess.com": self.chesscom_api,
            "www.chess.com": self.chesscom_signup,
            "gitlab.com": self.gitlab,
        }.get(host)
        if handler is None:
            return self.send(404, "no fake for " + host, ctype="text/plain")
        return handler(path, q, body)

    # Discord: webhooks, and the two sign-up checks (see check_discord in ringer.cpp).
    def discord(self, path, q, body):
        if path.startswith("/api/webhooks/"):
            return self.send(204)
        limited = '{"message":"You are being rate limited.","retry_after":%s,"global":false}'
        if "username-suggestions" in path:
            if "discord_limited" in SCENARIO:
                return self.send(429, limited % "2117.5")
            name = q["global_name"][0]
            return self.send(200, json.dumps({"username": name + "0288" if name in DISCORD_TAKEN else name}))
        if "discord_limited" in SCENARIO or "discord_attempt_limited" in SCENARIO:
            return self.send(429, limited % "2100.1")
        return self.send(200, json.dumps({"taken": json.loads(body)["username"] in DISCORD_TAKEN}))

    def roblox_batch(self, path, q, body):
        if "roblox_batch_fails" in SCENARIO and bump("roblox_batch") == 2:
            return self.send(500, '{"errors":[{"code":0,"message":"InternalServerError"}]}')
        data, seen = [], set()
        for name in json.loads(body)["usernames"]:
            if name.lower() in ROBLOX_TAKEN and name.lower() not in seen:  # the real one merges case variants
                seen.add(name.lower())
                data.append({"requestedUsername": name, "hasVerifiedBadge": False, "id": 1, "name": name.upper(),
                             "displayName": name})
        return self.send(200, json.dumps({"data": data}))

    def roblox_validate(self, path, q, body):
        name = q["Username"][0].lower()
        if name in ROBLOX_TAKEN:
            return self.send(200, '{"code":1,"message":"Username is already in use"}')
        if name in ROBLOX_BAD:
            return self.send(200, '{"code":2,"message":"Username not appropriate for Roblox"}')
        return self.send(200, '{"code":0,"message":"Username is valid"}')

    def minecraft_services(self, path, q, body):
        if path.endswith("/bulk/byname"):
            return self.minecraft_bulk(body)
        # Everything else needs a logged in player's token: a JWT whose payload says who and until when.
        try:
            payload = self.headers.get("Authorization", "").split(".")[1]
            claims = json.loads(base64.urlsafe_b64decode(payload + "=="))
            good = claims.get("sub") == "doobert" and claims.get("exp", 0) > time.time()
        except (IndexError, ValueError):
            good = False
        if not good:
            return self.send(401, '{"path":"%s"}' % path)
        if path == "/minecraft/profile":
            return self.send(200, '{"id":"abc","name":"Doobert1","skins":[],"capes":[]}')
        if "mc_available_429" in SCENARIO and bump("mc_available") == 1:
            return self.send(429, "")
        if "mc_available_bucket" in SCENARIO and not take_token():
            return self.send(429, "")
        name = unquote(path.split("/")[4]).lower()
        status = "DUPLICATE" if name in MC_TAKEN or name in MC_HELD else "NOT_ALLOWED" if name in MC_NOT_ALLOWED else "AVAILABLE"
        return self.send(200, json.dumps({"status": status}))

    def minecraft_bulk(self, body):
        names = json.loads(body)
        if len(names) > 10:
            return self.send(400, '{"error":"CONSTRAINT_VIOLATION"}')
        found = [{"id": "abc", "name": MC_TAKEN[n.lower()]} for n in names if n.lower() in MC_TAKEN]
        return self.send(200, json.dumps(found, indent=2))  # the real one pretty-prints too

    def minecraft_one(self, path, q, body):
        name = unquote(path.rsplit("/", 1)[1]).lower()
        if name in MC_TAKEN:
            return self.send(200, json.dumps({"id": "abc", "name": MC_TAKEN[name]}))
        return self.send(404, '{"errorMessage":"Couldn\'t find any profile"}')

    def lichess(self, path, q, body):
        if path == "/api/users":
            if "lichess_429" in SCENARIO and bump("lichess_batch") == 1:
                return self.send(429, "", ctype="text/plain")  # Lichess doesn't say how long
            found = []
            for name in body.decode().split(","):
                if name.lower() in LICHESS_TAKEN:
                    user = {"id": name.lower(), "username": name}
                    if name.lower() == "closedone":
                        user["disabled"] = True
                    found.append(user)
            return self.send(200, json.dumps(found))
        name = unquote(path.rsplit("/", 1)[1]).lower()
        return self.send(200 if name in LICHESS_TAKEN else 404, '{"id":"%s"}' % name)

    def github_api(self, path, q, body):
        good = self.headers.get("Authorization") == "Bearer " + GH_TOKEN
        if not good:
            return self.send(401, '{"message":"Bad credentials"}')
        if path == "/rate_limit":
            return self.send(200, '{"resources":{"core":{"limit":5000}}}')
        if path == "/graphql":
            return self.github_graphql(body)
        name = unquote(path.split("/")[2]).lower()
        if name == "primarylimit" and bump("gh_primary") == 1:
            return self.send(403, '{"message":"API rate limit exceeded for user ID 1."}',
                             {"x-ratelimit-remaining": "0", "x-ratelimit-reset": str(int(time.time()) + 3)})
        if name == "secondarylimit" and bump("gh_secondary") == 1:
            return self.send(403, '{"message":"You have exceeded a secondary rate limit."}', {"Retry-After": "2"})
        if name == "ratelimitbio":
            return self.send(200, json.dumps({"login": name, "bio": "my status: RATE_LIMITED lol"}))
        if name in GH_USERS or name in GH_ORGS:
            return self.send(200, json.dumps({"login": name}))
        return self.send(404, '{"message":"Not Found"}')

    def github_graphql(self, body):
        if "gh_graphql_ratelimited" in SCENARIO and bump("gh_graphql") == 1:
            return self.send(200, '{"errors":[{"type":"RATE_LIMITED","code":"graphql_rate_limit",'
                                  '"message":"API rate limit exceeded for user ID 1."}]}',
                             {"x-ratelimit-remaining": "0", "x-ratelimit-reset": str(int(time.time()) + 3)})
        if "gh_graphql_broken" in SCENARIO:
            return self.send(502, '{"message":"Server Error"}')

        def owner(_root, _info, login):
            if login.lower() in GH_USERS:
                return {"__typename": "User", "login": login.lower()}
            if login.lower() in GH_ORGS:
                return {"__typename": "Organization", "login": login.lower()}
            return None

        result = graphql_sync(GH_SCHEMA, json.loads(body)["query"], root_value={"repositoryOwner": owner},
                              field_resolver=lambda src, info, **args: src[info.field_name](src, info, **args)
                              if info.parent_type.name == "Query" else src.get(info.field_name),
                              type_resolver=lambda value, *_: value["__typename"])
        if result.errors:
            LOG.write(json.dumps({"graphql_errors": [str(e) for e in result.errors]}) + "\n")
            LOG.flush()
            return self.send(200, json.dumps({"errors": [{"message": str(e)} for e in result.errors]}))
        data = result.data
        if "gh_graphql_drops" in SCENARIO:
            data.pop(sorted(data)[-1])
        if "gh_errors_first" in SCENARIO:
            errors = [{"type": "NOT_FOUND", "path": [k], "message": "Could not resolve"} for k, v in data.items() if v is None]
            return self.send(200, json.dumps({"errors": errors, "data": data}))
        return self.send(200, json.dumps({"data": data}, separators=(",", ":")))

    def github_page(self, path, q, body):
        name = unquote(path.strip("/")).lower()
        return self.send(200 if name in GH_USERS or name in GH_ORGS else 404, "<html></html>", ctype="text/html")

    def chesscom_api(self, path, q, body):
        name = unquote(path.rsplit("/", 1)[1])
        if name != name.lower():
            return self.send(301, "", {"Location": "/api.chess.com/pub/player/" + name.lower()})
        if name in CC_TAKEN:
            return self.send(200, json.dumps({"username": name, "status": "basic"}))
        return self.send(404, '{"code":0,"message":"User not found."}')

    def chesscom_signup(self, path, q, body):
        if "cc_challenge" in SCENARIO and bump("cc_signup") >= 3:
            return self.send(429, "<!DOCTYPE html><title>Just a moment...</title>", {"cf-mitigated": "challenge"},
                             ctype="text/html")
        name = q["username"][0].lower()
        if name in CC_TAKEN:
            return self.send(200, '{"valid":false,"messages":["That username is taken. If this is you, \\u003Ca '
                                  'href=https:\\/\\/www.chess.com\\/login\\u003Elog in\\u003C\\/a\\u003E!"],'
                                  '"usernameSuggestions":[]}')
        if name in CC_BANNED:
            return self.send(200, '{"valid":false,"messages":["That username contains a word that is not allowed."],'
                                  '"usernameSuggestions":[]}')
        return self.send(200, '{"valid":true,"messages":["Username is valid"],"usernameSuggestions":[]}')

    def gitlab(self, path, q, body):
        name = unquote(path.split("/")[2]).lower()
        if name in GL_RESERVED:
            return self.send(302, "<html>redirected</html>", {"Location": "https://gitlab.com/users/sign_in"},
                             ctype="text/html")
        too_many = "This endpoint has been requested too many times. Try again later."
        if "gitlab_always_429" in SCENARIO or ("gitlab_429" in SCENARIO and bump("gitlab") == 2):
            return self.send(429, too_many, ctype="text/plain")
        return self.send(200, json.dumps({"exists": name in GL_TAKEN}))


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8765), Fake).serve_forever()
