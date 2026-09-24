# usercheck
to finally show your friends you have atleast something cool

Finds unclaimed usernames on Discord, Roblox, Minecraft, GitHub, or any site you give it a profile URL for, and can ping you on Discord when it finds one.

## Get it

**Windows, no compiling:** grab `usercheck.exe` from the [Releases](../../releases) page (or the `usercheck-windows` artifact on the latest [Actions](../../actions) run) and double-click it. Windows SmartScreen might warn about it because it isn't code-signed. Click "More info" then "Run anyway".

**Build it yourself:**

| OS | Command |
|---|---|
| Windows (Visual Studio) | Open the folder in Visual Studio (it picks up `CMakeLists.txt`), or in a Developer Command Prompt: `cl /std:c++17 /EHsc /O2 /utf-8 usercheck.cpp` |
| Windows (MinGW) | `g++ -std=c++17 -O2 -static usercheck.cpp -o usercheck.exe -lwinhttp` |
| Linux | `sudo apt install libcurl4-openssl-dev` then `g++ -std=c++17 -O2 usercheck.cpp -o usercheck -lcurl` |
| macOS | `clang++ -std=c++17 -O2 usercheck.cpp -o usercheck -lcurl` |

Or with CMake anywhere: `cmake -S . -B build && cmake --build build --config Release`.

Windows uses WinHTTP, which is built into Windows, so there's nothing extra to install. Linux and macOS use libcurl.

## Use it

1. Pick an app: Discord, Roblox, or Other (Minecraft, GitHub, or a custom site URL)
2. Pick what to check:
   - names from a `.txt` file (one per line, you can drag the file into the window)
   - random 3/4/5 letters or 3/4/5 characters (letters, numbers, and whatever symbols that app allows)
   - random custom length
3. Hits show up in green, beep, get saved to `available.txt`, and get posted to your Discord webhook if you set one up.

Ctrl+C stops a run early and still shows you what it found.

## Discord webhook pings

Pick **Discord webhook notifications** in the main menu:

1. In Discord: channel settings > Integrations > Webhooks > New Webhook > Copy Webhook URL
2. Paste it in and choose who gets pinged: nobody, @everyone, or one person by user ID
3. It sends a test message, and if that works the webhook is saved to `usercheck.cfg` for next time

Treat the webhook URL like a password. Anyone who has it can post in that channel. `usercheck.cfg` is in `.gitignore` so it won't get committed by accident.

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
