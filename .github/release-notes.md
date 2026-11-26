Run the installer (`brodalf-v0.10.1-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**Nothing new in this build has been tried by a person on a real Windows PC yet.** It was built and tested on Linux and by Windows CI, and every window, tab, menu and dialog was checked in automated screenshots on a Windows CI runner, in English, German and Spanish, light and dark. Please try it on a test folder first.

This build fixes twelve window problems that those screenshots showed in 0.10.0. It adds no new features. Everything settled before still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens.

**A backup you can follow**
- During a backup the line names the drive being written and how many are left, like "Backing up to Red (3 of 3)", and the full-copy bars move as each drive finishes instead of jumping at the end.
- Each full-copy bar says which drives make it up and gives one measure: "Full copy 1 of 2, on Blue: 2% of the data (7.0 MB of 315.9 MB), 43 of 86 files".

**Warnings that agree**
- The ransomware warning counts changed and deleted files apart, so its numbers match Next backup and the drive's details, and a click on it opens the list of changed files. Backing up to every drive now asks with **Show the changed files**, like backing up to one.
- A damaged file shows where you look: "may be damaged" in the file list, a line under the drives line, and a count on **Damaged files...**. The bit-rot scan's bar says what it found.
- A drive marked lost or broken says so in Status of every drive and in a file's versions, and is no longer listed as a place where files are kept in search results, Deleted files or the export.
- A drive's details no longer say its copies were read back and never read back at the same time.

**Restore from the lists that tell you to**
- **Damaged files**, **Changed since the copy on the drive** and **Left alone by the last restore** each have a button that restores the right version to a folder you choose. Nothing there is overwritten.

**Recovering the database**
- Recover also looks on the open database's backup drives, including drives that are folders. When it finds nothing, it offers **Look again** and **Choose the folder** instead of one OK that meant both.

**The window**
- The Primary Database tab's buttons no longer break into overlapping pieces when you select a file.
- A warning that wraps to a second line no longer prints over the full-copy bars.
- Dark mode: list column headers are readable.

**German and Spanish**
- The encryption question, the file types in Save dialogs, "no copy yet" and the notes in the default leave-out list are translated, and grammar is fixed in both. The engine's own log lines, like "scanning C:\…", are still in English.

0.10.1 opens databases made by 0.10.0 and older, and 0.10.0 can still open them afterwards: the database format is unchanged.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
