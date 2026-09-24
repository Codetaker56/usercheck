# ringer (formerly usercheck)
to finally show your friends you have atleast something cool

Finds unclaimed usernames on Discord, Roblox, Minecraft, GitHub, Lichess, Chess.com, GitLab, or any site you give it a profile URL for, and can ping you on Discord when it finds one.

ringer used to be called **usercheck**. Same tool, new name and a new look. If you have a `usercheck.cfg` from back then, ringer still reads your webhook settings from it.

```
 +----------------------------------------------------------------------------+
 | ringer                                                                     |
 +----------------------------------------------------------------------------+

                          ';>-}1)))}_l               _
                     'i]\nvrjrftf\jzXXv-       _____(_)___  ____ ____  _____
                  !}jnnzJLwh*****amj{cXX+     / ___/ / __ \/ __ `/ _ \/ ___/
              '<\YLQLOwZZqCzzCp*****Y{XX{    / /  / / / / / /_/ /  __/ /
            !(Uqahbh*#aqQUv]'  _h***01XX;   /_/  /_/_/ /_/\__, /\___/_/
         '-xUZdho#&%%WapmZOJ/:  w**o(cX-                 /____/
       `}vrzqoM&B@@8MakbbbwCn)^)**ajuz~           find unclaimed usernames
      -vrzhwpoMWWM*oaoo*adOYntzo*qfcr;
    ;rcfq*oX0wdkhao#MM*hq0UzJk*hzrv-        +- Pick an app -------------------+
   ~zuja**):Jmdka**ohbw0CJOk*bYrv}`         | [1]  Discord                    |
  -Xc(o**w  !uLOZmZOQLCZdo*Zcnx-'           | [2]  Roblox                     |
 ;XX10***h_  '_fucYQwbo*mYxu(!              | [3]  Other apps                 |
 {XX{Y*****pCzzCqa**bLXxu|<'                | [4]  Webhook pings          off |
 +XXc{jma*****hwLJznnj}!                    |                                 |
  -vXXzj\ftfrjrvn\]i'                       | [0]  Quit                       |
    l_})))1}->;'                            +---------------------------------+
```

## Get it

**Windows, no compiling:** grab `ringer.exe` from the [Releases](../../releases) page (or the `ringer-windows` artifact on the latest [Actions](../../actions) run) and double-click it. Windows SmartScreen might warn about it because it isn't code-signed. Click "More info" then "Run anyway".

**Build it yourself:**

| OS | Command |
|---|---|
| Windows (Visual Studio) | Open the folder in Visual Studio (it picks up `CMakeLists.txt`), or in a Developer Command Prompt: `cl /std:c++17 /EHsc /O2 /utf-8 ringer.cpp` |
| Windows (MinGW) | `g++ -std=c++17 -O2 -static ringer.cpp -o ringer.exe -lwinhttp` |
| Linux | `sudo apt install libcurl4-openssl-dev` then `g++ -std=c++17 -O2 ringer.cpp -o ringer -lcurl` |
| macOS | `clang++ -std=c++17 -O2 ringer.cpp -o ringer -lcurl` |

Or with CMake anywhere: `cmake -S . -B build && cmake --build build --config Release`.

Windows uses WinHTTP, which is built into Windows, so there's nothing extra to install. Linux and macOS use libcurl.

**Making a new release:** Actions tab > build > Run workflow, type a tag like `v1.1.0`, and it publishes the .exe to Releases.

## Use it

Every screen has a header showing where you are (like `ringer > Discord > Random 4 letters`). Type the number next to what you want and press Enter. `0` goes back, or quits from the main screen.

1. Pick an app: Discord, Roblox, or Other apps (Minecraft, GitHub, Lichess, Chess.com, GitLab, or a custom site URL)
2. Pick what to check:
   - names from a `.txt` file (one per line, you can drag the file into the window)
   - random 3/4/5 letters or 3/4/5 characters (letters, numbers, and whatever symbols that app allows)
   - random, any length you pick

   Then how many seconds to wait between requests. Each app has a sensible default, just press Enter.
3. Watch the results scroll past with a progress bar underneath showing how far along it is, how many it's found, and roughly how long is left. If the site rate limits you, the bar counts down the wait (see [Rate limits](#rate-limits)).
4. Hits show up in green, beep, get saved to `available.txt`, and get posted to your Discord webhook if you set one up.
5. When it's done you get a summary card: how many were available, taken, not allowed, or errored, how long it took, and the names it found.

Ctrl+C stops a run early and still shows you the summary. If you were checking names from a `.txt` file, the ones it didn't get to are saved to `unchecked.txt`, so you can load that next time and carry on where you left off.

ringer only prints plain ASCII, so it looks right in every Windows console font, including the old raster fonts. Colors need Windows 10 or newer. On older Windows it runs in plain black and white. It's laid out for an 80-column window, and if the window is narrower it drops the Saturn and keeps the rest.

## Discord webhook pings

Pick **Webhook pings** on the main screen:

1. In Discord: channel settings > Integrations > Webhooks > New Webhook > Copy Webhook URL
2. Paste it in and choose who gets pinged: nobody, @everyone, or one person by user ID
3. It sends a test message, and if that works the webhook is saved to `ringer.cfg` for next time

Treat the webhook URL like a password. Anyone who has it can post in that channel. `ringer.cfg` (and the old `usercheck.cfg`) are in `.gitignore` so they won't get committed by accident.

If there's no `ringer.cfg` yet, ringer reads `usercheck.cfg` instead. The first time you change a webhook setting, ringer writes `ringer.cfg` and uses that from then on. The old file is left alone, so delete it yourself once you've moved over, because it still has your webhook URL in it.

## GitHub token

Without a token, ringer checks GitHub by loading profile pages, one name at a time. With one, it uses GitHub's API instead and looks names up 100 per request.

1. On github.com: Settings > Developer settings > Personal access tokens > Fine-grained tokens > Generate new token. It doesn't need any permissions, the defaults are fine.
2. In ringer: Other apps > GitHub token > Set token, and paste it in
3. ringer checks it with GitHub, and if it works it's saved to `ringer.cfg`

Treat the token like a password. Why not always use the API? Without a token it only allows 60 checks an hour, which is way slower than loading profile pages.

If a batch lookup ever fails, ringer checks that batch one name at a time with GitHub's regular API (5,000 an hour) instead, so a run never stalls on it.

## Rate limits

Every site limits how fast you can check names. When one tells ringer to back off, ringer waits exactly as long as the site asks and then keeps going by itself. The progress bar counts down the wait. If a site doesn't say how long, ringer waits 15 seconds, then 30, then a minute, and so on up to 10 minutes.

**Discord is the harsh one.** Its sign-up check only lets each internet connection check about 20 names before it makes you wait, and that wait can be over half an hour (one run got told to wait 2117 seconds). No setting in ringer changes that, it's Discord's limit. Lowering "seconds between checks" just gets you to the wait faster.

So ringer uses two of Discord's sign-up endpoints, each with its own limit:

1. **Suggestions** (the one that offers you a username when you sign up). If the name you ask about is free, it hands the same name back. If it's taken, it suggests something else like `abcd.` or `abcd0288`. Every name goes through this first. It allowed about 37 checks before a ~36 minute wait in testing.
2. **The real sign-up check.** Only names that look free get double-checked here, so its ~20 checks aren't wasted on names that are obviously taken.

When one of them is rate limited, the other keeps going on its own. If the double-check is the one waiting, hits still show up but are marked **not double-checked** (`abcd?` in the summary, `(not double-checked)` in `available.txt` and the webhook ping). In testing the suggestions check never called a taken name free, but it isn't the check Discord actually enforces, so treat those as very likely rather than certain.

Put together that's roughly 2-3x the names per half hour compared to the sign-up check alone. Still not fast: plan on something like 50-60 names per half hour, and big runs taking hours.

What ringer does about it:

- Long waits get a heads-up with the time it will carry on, like `Discord rate limited you for 35m 17s. Carrying on by itself at 14:32.` Leave the window open and it keeps going.
- Long waits are remembered in `ringer.cfg`. If you close ringer and open it again, the main menu shows `Discord  limited 31m 05s` so you know before you start.
- Ctrl+C during a wait stops cleanly, shows what it found, and (for `.txt` file runs) saves the rest to `unchecked.txt`.

The only ways around Discord's limit are rotating through proxies or checking through a logged-in account's token. Both break Discord's rules and can get the IP or the account banned, so ringer doesn't do either.

**Roblox, Minecraft, Lichess and GitHub** (with a token) look names up in batches: 100, 10, 300 and 100 per request. Names the lookup finds are taken without another request, so dense name lists (like 4 letters, where nearly everything is taken) fly by. On Roblox, anything the lookup doesn't find still gets checked with sign-up validation one at a time, so hits are slower than misses.

**Chess.com** has two checks, like Discord. Its public API is the main one and doesn't mind a steady stream of requests. Names it has no account for get double-checked with the sign-up form's check, which also catches banned words, but that one only allows about 4 checks before a roughly one minute wait. While it's waiting, hits are marked **not double-checked**. The public API counts closed accounts, so those hits are still very likely free.

**GitLab** allows about 20 checks a minute, so ringer waits 3 seconds between them by default.

## How it checks

| App | How | How much to trust "available" |
|---|---|---|
| Roblox | Roblox's user lookup, 100 names per request, then sign-up validation for anything it doesn't find | High, it's the same check sign-up uses |
| Discord | Discord's sign-up username suggestions, then its sign-up "is this taken" check for anything that looks free. No login or token needed | High when double-checked. Hits marked "not double-checked" are very likely free but only the first check saw them |
| Minecraft | Mojang's bulk profile lookup, 10 names per request | Medium, recently changed and banned names show as free |
| GitHub | GitHub's API if you've set a token (100 names per request, users and organizations), otherwise the profile page | Medium, reserved/deleted names come back as not found too |
| Lichess | Lichess' user lookup, 300 names per request. Closed accounts count as taken | High, Lichess never frees a name. It does turn down some offensive names at sign-up that ringer can't see |
| Chess.com | Chess.com's public API, then the sign-up form's check for anything with no account | High when double-checked (it catches banned words too). "Not double-checked" hits have no account, closed ones included |
| GitLab | The check GitLab's sign-up form uses. Covers groups too, since they share names with users | High. Names GitLab keeps for its own pages show as not allowed |
| Custom | Profile URL 404 | Depends on the site, test with a name you know exists first |

## Reality check

Every 3-letter and 3-character name on Discord and Roblox is gone, and so is basically every 4-letter one. Running those modes will mostly show `taken`. 4-character names with numbers/symbols turn up the odd hit, and 5-letter gibberish is wide open on Discord.

From test runs in September 2026, some places that aren't picked clean yet:

- Roblox: about 1 in 3 random 5-character names
- Lichess: over half of random 4-letter names
- GitLab: about 3 in 4 random 4-letter names
- Minecraft and Chess.com: most random 5-letter names
