Run the installer (`brodalf-v0.15.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**What is new in this build has not been tried by a person on a real Windows PC yet.** It was built and tested on Linux and by Windows CI, which now also runs it on real FAT32 and exFAT volumes, a network share and the window app itself. Please try it on a test folder first.

This build fixes what an independent walk through every flow of 0.14.0 found: 40 findings, and five gaps in how BRODALF is tested. Everything settled before still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens.

**Shadow copies work again**
- In every Windows build of 0.14.0 a shadow copy stopped with "cannot ask programs to get ready (Windows error 0x80004005)", so files in use, like an open Outlook mailbox, could not be backed up that way. They can again, and CI now checks it on every push.

**Restoring without BRODALF**
- Every drive with a plain database copy now also gets `newest-copies-r<number>.txt` beside it: for each backup root, where the newest copy of every file is on that drive, and which files have a newer version, a new name, are deleted or sealed. The recovery sheet now says the same: the first copy of a file on a drive is its oldest version, not its newest.

**Scanning and backup roots**
- A file changed with its size and modified time kept as they were (VeraCrypt containers do this) is now caught by its change time and backed up. The bit-rot scan no longer advises restoring the old copy over such a file.
- A missing backup root is pointed at another folder without asking only when that folder surely holds it (at least 15 of 16 sampled files at their paths with their sizes, and the smallest of them with the right contents), and never when the root is set to wait for its disk. Otherwise every folder that may hold it is offered, two or more included.
- A stopped scan keeps what it did, and a root that never finished a scan is counted as not yet backed up.
- The default skip list now leaves out Windows' own busy profile files (`NTUSER.DAT`, `UsrClass.dat`) and the caches and temporary files in `AppData`. In a root scanned before with the default list, those files are marked deleted at the next scan; their copies stay.
- A backup root on a network share can be set to wait for it, like one on a USB disk.
- Adding the folder that holds a backup root offers to take the root in, so nothing is copied again.
- The new-database steps check the folders before the save dialog. Skip patterns ignore case for every letter, and `?` matches one letter. A file name that is not valid Unicode is now named in the log.

**Backing up**
- A backup leaves out a root the scan held back or could not find, and says so, instead of recording an unrelated file as its newest version.
- On a drive too small for everything, starred files go first.
- A backup that stops early, or loses one of several drives, still puts a database copy on every drive still there, so each drive knows what was copied to it. A database copy that cannot be written counts as a problem, not a log line, and the room kept for it is kept.
- The room a copy takes is counted in the drive's own allocation units, so a drive of many small files does not fill up by surprise. A full drive is said to be full at once, without reading the rest of the files first.
- One file in an encrypted folder no longer blocks a whole backup when the passphrase is not entered: everything else is backed up and those files wait.
- "Up to date" now says how old the scan behind it is, and offers to scan first when it is old.
- Copies whose path on the drive is longer than Explorer opens are counted, and the log says how many.

**Drives**
- Two copies of one database used apart can reach the same revision. The later one now goes on the drive beside the other, with a tag in its name, and BRODALF says the drive holds two. Before, it was left out without a word and that drive stopped getting database copies.
- A drive whose disk looks different from the one set up (a cloned drive, another serial number) is asked about before its copies count.
- Marking a drive lost or broken says what it costs: only current files are backed up again, and older versions and deleted files that only it held are not.
- A wiped drive set up again can be taken as the same drive, its old copies marked missing (`brodalf-cli drive ... --wiped NAME`), so they no longer count.
- A new passphrase or recovery key goes into a database copy on every plugged-in drive at once (`--drive ROOT` on the command line). Making a recovery key asks where to save its sheet first, and saving a sheet with the key into a backup root, from where it would be backed up in plain text, asks before it does.
- **Found it** offers to check the drive. A drive kept as a folder on a NAS or share is looked for in other places too. Drive names are kept unique.

**Restoring and search**
- A restore that meets files already there compares them, says "already restored" only when they are the same, and can keep both.
- A damaged copy whose good twin is on an unplugged drive waits for that drive, and the window no longer says the restore is complete.
- Plain files are restored before the passphrase is asked for. A full or read-only restore folder is named as the problem and stops the restore, instead of the backup copy being blamed.
- Restoring a missing root offers to make the restored folder the root. Restoring across several drives does not read again what is already back, and the plan is kept when the database closes.
- Restoring into a backup root or a drive's BRODALF folder asks first.
- Search finds letters of any case, accented capitals included, takes `*` and `?`, shows the drives of every version and a date column, and runs while a scan does.

**Files at risk**
- "Which drive helps" leaves out a drive on the same physical disk as one that already holds the files, one on the files' own disk and a failing one, since none would add a copy that counts.

**The command line**
- New exit codes for scripts and the Task Scheduler: 4, not possible now and nothing done (the drive is not plugged in, or the catalog is open elsewhere), and 5, many files changed at once and nothing was backed up.
- `backup --plugged` backs up to every drive of the catalog that is plugged in, `--accept-changes` backs up after many files changed, `wait` sets a root to wait for its disk or share, and `add --take-in` takes in the roots inside a folder.
- `--recovery-key` (or `BRODALF_RECOVERY_KEY`) opens a database with the recovery key, and `recover` falls back to an older copy when the newest does not open.
- The command line refuses to open a database copy on a backup drive, which would change it, and says how to recover it instead.
- `BRODALF_PASSPHRASE` is read as Unicode on Windows, and a mistyped option is refused.

**Testing**
- CI now drives the window app through its tabs, menus, dialogs and jobs on every push, runs the checks that used to be skipped on Windows, reads the runner's real disk details, makes real FAT32 and exFAT volumes and a share for the tests, and takes a real shadow copy.

0.15.0 opens databases made by 0.14.0 and older, and 0.11.0 and 0.10.2 can still open them afterwards: new facts go in new tables and settings, and the database format number is unchanged. 0.14.0 and older do not see a tagged database copy when recovering.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
