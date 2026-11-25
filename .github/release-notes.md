Run the installer (`brodalf-v0.6.0-windows-x64-setup.exe`): it needs no administrator rights and adds BRODALF to the Start menu. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**Nothing new in this build has been tried on a real Windows PC yet.** It was built and tested on Linux and built by Windows CI; please try it on a test folder first.

**Nothing starts by itself any more.** BRODALF keeps track of what has been backed up and to which drive, and does a scan, backup, check or restore only when you press its button. Taken out:
- The daily or weekly scheduled check. Opening a catalog deletes the Windows task that 0.5.0 or earlier created for it.
- Backing up when a drive is plugged in.
- Reading back copies when a drive is plugged in.
- Continuing a waiting restore when the next drive is plugged in.
- Reminders on opening that a drive is due for a check.
- The ransomware guard that paused backups after a mass change (the backup preview warns instead).
- Reading files in use from a shadow copy (BRODALF now lists them and asks you to close the program and scan again).
- Rescanning the folders on opening, and the quick check on plug-in. Both are now buttons: **Scan folders**, and **Check drive…**, which offers a quick check, a full check or a read back.

Plugging in a known drive still lights up the files on it, and the log says when it is due for a check, holds files for a waiting restore or can take what did not fit elsewhere.

New in this build:
- **Before a backup**, BRODALF shows what it will do: files to copy and how much, files to move, old versions to keep or remove, the drive's free space and whether it fits, and when the folders were last scanned. Start, scan first, or cancel. If many backed-up files changed or vanished since, it warns and keeps every old version.
- **Where each drive stands**: files whose newest version, an older version or no version is on it, deleted files and old versions kept there, bad or never-checked copies, and its last backup and check. In the drive's details, and for all drives in **Local backups > Status of every disk…**.
- **History** of every scan, backup, restore and check, with when, how long and the result.
- **Export the file list (CSV)…** with every file and the drives it is on.
- **Files that were in use at the last scan…**.
- A waiting restore continues when you press **Restore…** and choose to continue.
- OneDrive files kept online only are left alone and not downloaded.
- `brodalf-cli` gains `preview`, `status`, `history`, `export` and `backup --keep-old`, and Ctrl+C stops a job and keeps what finished.

Left out of this build: a read back right after each copy (Windows reads it from memory, so it proves nothing; Check drive's read back does the real check), adopting an existing drive, copying from one drive to another, and backing up creation times and file attributes. Each needs larger changes that could not be tested here.

0.6.0 opens the same catalogs as 0.5.0.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
