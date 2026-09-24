# Changelog

Each `## vX.Y.Z` section here becomes the notes for that GitHub release.

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
