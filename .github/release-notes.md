Run the installer (`brodalf-v0.7.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**Nothing new in this build has been tried on a real Windows PC yet.** It was built and tested on Linux and built by Windows CI; please try it on a test folder first.

This build follows the answers to 178 yes/no questions about what BRODALF should do. BRODALF keeps track of what has been backed up, and to where, from your backup roots (the folders you back up).

**Nothing on a backup drive is ever overwritten, moved or deleted.**
- A changed file's new version goes alongside the old copy, under `.versions`, as `name.v2.ext` and so on. Old versions are never removed.
- A moved or renamed file or folder is recorded in the database; the copy on the drive stays where it is.
- Every backup leaves a copy of the database on the drive as `catalog-r<revision>.brodalf`, next to the older ones, and keeps 512 MB free for it (Settings).
- Restores never overwrite either: a file already in the destination is skipped if it matches, and left alone and listed if it differs (**Restore > Files the last restore left alone**).

**Starting up**
- Until a database is open, only **New database**, **Open database** and **Recover the database from a backup drive** are available.
- A new database asks for its backup roots first, then where to save the file.
- Recover looks on every drive for BRODALF's database copies and offers the newest.
- Opening a database starts a scan straight away. This is the one thing BRODALF starts by itself. If a backup root is not where the database says, BRODALF asks where it is now, and offers to restore it from your drives if you cannot find it.

**Backing up**
- One line beside the buttons says what a backup to each plugged-in drive would copy. **Back up** copies it without a preview window.
- If many files changed at once, BRODALF asks first and can list them, since that is what ransomware looks like.
- A file that changed since the scan is read again.
- If a drive fills up, what did not fit waits for another drive, and each drive gets whatever it is missing.
- Jobs let other programs come first by default (Settings).

**New**
- Progress bars for each full copy of your files, the running job and the file being copied.
- Menus: File, Database, Local backups, Restore, View and Help, plus right-click menus on the tree and the versions.
- Backup roots can be stopped and resumed, have their own list of what to leave out, and can be pointed at a new place.
- When files are deleted, BRODALF says so and lets you keep tracking them or forget them. Their copies stay on the drives either way.
- **Database > Bit-rot scan** finds files damaged on this PC; **Damaged files** lists them to restore or keep.
- Mark a drive lost or broken (after seeing what that does to each full copy), and Found it if it turns up.
- **View > Largest files**, **Restore > Restore deleted files**, and restoring one version from the versions list.
- When a disk BRODALF does not know is plugged in, it offers to set it up as a backup drive, or not to ask about that disk again.
- `brodalf-cli` gains `roots`, `root-stop`, `root-resume`, `root-move`, `deleted`, `deleted-ack`, `deleted-forget`, `drive-lost`, `drive-found`, `changed`, `sets`, `largest`, `restore-version`, `restore --deleted`, `scan --bitrot`, `recover`, `recovery-key`, `sheet` and `--json`. `--source` is now `--root` (the old name still works).

**Taken out**
- The job history (`history`), restoring as it was on a date (`--as-of`), and the old-versions rule (`prune`, `--keep-old`): nothing is removed from a drive any more.
- Replace this drive, and `--continue-from`: every drive now gets whatever it is missing.
- The quick check: Check drive always compares each copy's size, time and checksum.

**Left for later:** tabs in place of some menus, dark mode, translations, a cross-platform app (after 1.0), encrypting only some folders, starred folders copied to extra places, copying to several drives at once, a compression helper and reading files in use from a shadow copy.

0.7.0 opens and upgrades databases made by 0.6.0 and older, and recovers from their `catalog-backup.brodalf` copies on drives. After that, 0.6.0 cannot open the database.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
