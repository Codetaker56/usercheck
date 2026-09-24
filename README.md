# usercheck
to finally show your friends you have atleast something cool

Finds unclaimed usernames on Discord, Roblox, Minecraft, GitHub, or any site you give it a profile URL for.

## Run it

Needs Python 3.8+ ([python.org](https://www.python.org/downloads/)). Nothing else to install.

```
python usercheck.py
```

1. Pick an app: Discord, Roblox, or Other (Minecraft, GitHub, or a custom site URL)
2. Pick what to check:
   - names from a `.txt` file (one per line)
   - random 3/4/5 letters or 3/4/5 characters (letters, numbers, and whatever symbols that app allows)
   - random custom length
3. Hits show up in green, beep, and get saved to `available.txt`.

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
