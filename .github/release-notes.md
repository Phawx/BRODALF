Unzip it anywhere and run `brodalf.exe`; there is no installer yet.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed yet. Click **More info**, then **Run anyway**.

New in this build:
- BRODALF no longer needs to run all the time: a daily (or weekly) scheduled check rescans your folders without a window and says how much needs backing up and which known disk has room for it
- Every disk's free space is recorded when it is plugged in and before and after each backup
- Menu bar with **Local backups** (each external or removable disk, with its own submenu) and **Cloud backups** (active connections)
- Moved or renamed folders are recognised by checksum and moved on the drive instead of copied again
- Plugging in a known disk reads back the copies not read back in the last month and checks their checksums
- A progress bar with counts, data rate and the current file
- Ransomware guard: when a quarter or more of your files change at once, backups pause and BRODALF offers to restore everything as it was before; restoring any folder as of a date
- Files in use (Outlook `.pst`, databases) can be read from a Windows shadow copy, with one administrator prompt

Still to be tried on a real PC (this build was only tested under Wine): the shadow copy itself, the scheduled task, and drives being plugged in while BRODALF is open.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
