Unzip it anywhere and run `brodalf.exe`; there is no installer yet.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed yet. Click **More info**, then **Run anyway**.

New in this build:
- OneDrive and Dropbox storage has been taken out for now. Everything about external and removable disks is unchanged. A catalog that had a cloud account still opens; the account is shown as unsupported and the copies it holds still count.
- Folders inside OneDrive or Dropbox are scanned properly: their files carry a Windows marker that earlier builds could mistake for a shortcut and skip.
- A copy found damaged by a full check now stays marked damaged until it is read back correctly. Before, the quick check at the next plug-in could clear the mark without reading the file.
- Reading files that are in use from a shadow copy no longer risks hanging (the helper now starts in the right COM mode).
- The **Files at risk** list said "changed since its last backup" for files whose newest version was already backed up.

This is the first release after BRODALF was tried on a real Windows PC (0.3.0, 2026-10-04): scanning, backing up to a USB stick, a full check, changing, renaming and removing files, old versions and restoring all worked first time. Still to be tried: the scheduled check's message, the shadow copy itself, plugging a disk in while BRODALF is open, and SMART data from internal disks.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
