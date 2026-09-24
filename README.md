# ringer
to finally show your friends you have atleast something cool

Finds unclaimed usernames on Discord, Roblox, Minecraft, GitHub, or any site you give it a profile URL for, and can ping you on Discord when it finds one.

ringer used to be called usercheck. If you have a `usercheck.cfg` from back then, ringer still reads your webhook settings from it.

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

1. Pick an app: Discord, Roblox, or Other apps (Minecraft, GitHub, or a custom site URL)
2. Pick what to check:
   - names from a `.txt` file (one per line, you can drag the file into the window)
   - random 3/4/5 letters or 3/4/5 characters (letters, numbers, and whatever symbols that app allows)
   - random, any length you pick
3. Watch the results scroll past with a progress bar underneath showing how far along it is, how many it's found, and roughly how long is left. If the site rate limits you, the bar counts down the wait.
4. Hits show up in green, beep, get saved to `available.txt`, and get posted to your Discord webhook if you set one up.
5. When it's done you get a summary card: how many were available, taken, not allowed, or errored, how long it took, and the names it found.

Ctrl+C stops a run early and still shows you the summary.

ringer only prints plain ASCII, so it looks right in every Windows console font, including the old raster fonts. Colors need Windows 10 or newer. On older Windows it runs in plain black and white. It's laid out for an 80-column window, and if the window is narrower it drops the Saturn and keeps the rest.

## Discord webhook pings

Pick **Webhook pings** on the main screen:

1. In Discord: channel settings > Integrations > Webhooks > New Webhook > Copy Webhook URL
2. Paste it in and choose who gets pinged: nobody, @everyone, or one person by user ID
3. It sends a test message, and if that works the webhook is saved to `ringer.cfg` for next time

Treat the webhook URL like a password. Anyone who has it can post in that channel. `ringer.cfg` (and the old `usercheck.cfg`) are in `.gitignore` so they won't get committed by accident.

If there's no `ringer.cfg` yet, ringer reads `usercheck.cfg` instead. The first time you change a webhook setting, ringer writes `ringer.cfg` and uses that from then on. The old file is left alone, so delete it yourself once you've moved over, because it still has your webhook URL in it.

## How it checks

| App | How | How much to trust "available" |
|---|---|---|
| Roblox | Roblox's sign-up validation endpoint | High, it's the same check sign-up uses |
| Discord | Discord's sign-up "is this taken" endpoint, no login or token needed | High, but Discord can still refuse a name when you actually set it |
| Minecraft | Mojang profile lookup | Medium, recently changed and banned names show as free |
| GitHub | Profile page 404 | Medium, reserved/deleted names 404 too |
| Custom | Profile URL 404 | Depends on the site, test with a name you know exists first |

## Reality check

Every 3-letter and 3-character name on Discord and Roblox is gone, and so is basically every 4-letter one. Running those modes will mostly show `taken`. 4-character names with numbers/symbols turn up the odd hit, and 5-letter gibberish is wide open on Discord.
