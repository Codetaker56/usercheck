# Changelog

Each `## vX.Y.Z` section here becomes the notes for that GitHub release.

## v2.4.0

- **New: Every app.** Type a name, or a few, and ringer checks each one on Discord, Roblox, Minecraft, GitHub, Lichess, Chess.com and GitLab, then sums up where each one is free.
- Rate limits of a minute or less get waited out. Longer ones skip that app for the rest of the run, so a Discord wait doesn't hold up the others.
- Hits get saved to `available.txt`, and webhook pings come one per name, listing every app it's free on.
- Webhook pings moved from 4 to 5 on the main menu.

## v2.3.0

- **GitHub with a token looks names up 100 at a time** using GitHub's GraphQL API. Each lookup counts as one of the 5,000 an hour, so in practice the limit stops mattering. It checks organizations too, since they share names with users.
- If a batch lookup ever fails, that batch is checked one name at a time with the regular API instead, so runs don't stall.
- Without a token, nothing changes: GitHub is still checked with profile pages.

## v2.2.0

Much faster Roblox and Minecraft, a GitHub token option, and three new apps.

- **Roblox** looks names up 100 at a time. Anything that isn't an existing account still goes through sign-up validation, so hits are exactly as reliable as before. In testing, 300 random 4-character names took 2 seconds instead of about 3 minutes.
- **Minecraft** looks names up 10 at a time with Mojang's bulk lookup. 100 names took 11 seconds instead of well over a minute and a half.
- **GitHub** can use GitHub's API with a token (Other apps > GitHub token), which allows 5,000 checks an hour. Without a token it keeps loading profile pages, because the API only allows 60 checks an hour without one.
- **New: Lichess.** Looks names up 300 at a time. Closed accounts count as taken, since Lichess never frees a name.
- **New: Chess.com.** Checks with the public API, then double-checks hits with the sign-up form's check, which also catches banned words. That one only allows about 4 checks a minute, so hits past that are marked "not double-checked".
- **New: GitLab.** Uses the check GitLab's sign-up form does, so group names and names GitLab reserves are caught too. GitLab allows about 20 of these a minute, so it waits 3 seconds between checks by default.
- "Seconds between checks" is now "seconds between requests". Names a batch lookup already answered don't wait.
- Turning off webhook pings no longer wipes the rest of `ringer.cfg` (like remembered rate limits or the GitHub token).

## v2.1.0

More Discord checks per half hour.

- Discord names are now checked with Discord's sign-up username suggestions first, which has its own rate limit, and only names that look free get double-checked with the sign-up check ringer used before. Together that's roughly 2-3x as many names per half hour (about 37 + 20 before the long wait, instead of 20).
- If the double-check is rate limited, hits still show up but are marked "not double-checked" (`name?` in the summary, `(not double-checked)` in `available.txt` and webhook pings) instead of holding up the run.
- If either check is rate limited the other keeps going on its own. ringer only waits when both are.

Discord is still slow. Plan on something like 50-60 names per half hour.

## v2.0.0

**usercheck is now ringer.** Same tool, new name and a new look. Download `ringer.exe` below. If you have a `usercheck.cfg` from before, ringer still reads your webhook settings from it.

New look:

- Saturn on the main screen next to the title
- a header on every screen showing where you are, like `ringer > Discord > Random 4 letters`
- menus in boxes, with `0` to go back
- a live progress bar during runs with how many it's found and roughly how long is left
- a summary card at the end of every run
- plain ASCII only, so it looks right in every Windows console font. Gray text is actually gray in the classic Windows console now

Rate limits:

- waits get a live countdown on the progress bar instead of a frozen `rate limited, waiting 2117s...` line
- long waits tell you the time ringer will carry on, and get remembered, so after a restart the main menu shows which apps are still limiting you
- sites that rate limit without saying for how long get an increasing wait (15s, 30s, 1m... up to 10m) instead of being hit again every 10 seconds
- stopping a `.txt` file run with Ctrl+C saves the names it didn't get to in `unchecked.txt`

Discord still only allows roughly 20 checks per half hour per connection. That's Discord's limit and there's no legit way around it, see the README.

## v1.0.0

First release, as usercheck.
