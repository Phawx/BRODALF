First build to try. Unzip it anywhere and run `brodalf.exe`; there is no installer yet.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed yet. Click **More info**, then **Run anyway**.

What's in it:
- Protected folders, scanning with checksums, and the "ghost" tree that lights up files once a good copy is on a plugged-in drive
- Backup drives identified by an ID file, with make, model, serial, health and where each drive is kept
- Old versions kept in `.versions` on each drive, with a keep rule
- Checks, restores, and restore plans across several drives
- Spreading a big folder across drives when one fills up
- Optional encryption, Dropbox (OneDrive once its app ID is set up)
- At-risk view, search, backup on plug-in, check reminders, skip lists
- Error reports saved to `%LOCALAPPDATA%\BRODALF\reports`, to post as an issue

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
