Run the installer (`brodalf-v0.10.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**Nothing new in this build has been tried on a real Windows PC yet.** It was built and tested on Linux and built and tested by Windows CI, and the changed window has not been looked at; please try it on a test folder first.

This build fixes what two independent reviews of 0.9.0 found. It adds no new features. Everything 0.9.0 settled still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens; and copies are plain files in their folders unless you choose otherwise.

**A drive that drops out is no longer written off**
- If a drive is unplugged or stops answering during a check or a backup, BRODALF now stops that drive's job and says so, instead of marking every copy it could not reach as missing and copying them all again next time. Copies are only marked missing when the drive is there and answering and the file really is not on it.

**Restores**
- A restore tries every good copy on the drives that are plugged in, not just the first. A copy that does not read back right is marked damaged, and the next one is tried.
- When a file's newest contents were never backed up (after ransomware, or a mass edit that was scanned before you noticed), a restore now brings back the newest version that does have a copy, instead of reporting "never backed up". The summary says how many files came back as an older version.
- Restore and recovery refuse a path stored in the database that would lead outside the folder you chose.

**Checking copies**
- A copy that has never been read back from its drive is now due for a check a week after it was written, instead of after 180 days, and the versions list says "not read back yet". BRODALF offers to check just those copies; it never starts by itself.

**Safer and faster**
- Each drive's status is worked out in one pass instead of one search per file, so large databases no longer freeze the window (100,000 files: from not finishing to about a third of a second).
- The database is saved every few minutes during a long backup, so a crash or power cut no longer loses the record of a first backup of many hours. It is not saved when nothing changed.
- A folder that cannot be read to the end (a network share dropping, a USB source pulled) is reported, and the files in it are no longer marked deleted. File names that cannot be converted are reported too.
- The mass-change warning now also counts files that were renamed and then deleted, the usual pattern of ransomware.
- A backup root inside another root, or holding one, is refused, so no file is tracked or counted twice.
- On FAT32 drives, files of 4 GB or more are left out of the backup with a plain reason, instead of being copied up to the limit and failing every time.
- What the bit-rot scan found is kept in the database until it is put right, instead of being lost when BRODALF closes.
- Working copies of the database that a crashed BRODALF left in the temp folder are removed the next time it starts. While an encrypted database is open, its working copy in the temp folder is still unencrypted; that is unchanged.
- Network-share paths get Windows' long-path prefix too, and a path with `.` or `..` in it is resolved first.
- The CSV export turns `/` into `\` only in the path columns.
- The elevated shadow-copy helper copies only files that the person who asked may read.

**The window**
- **Keyboard:** every button, tab and menu can be reached with Tab, arrow keys and an underlined letter. New shortcuts: Ctrl+N and Ctrl+O (new and open database), Ctrl+B (back up), F5 (scan), Ctrl+F (search), Ctrl+1 to Ctrl+6 and Ctrl+Tab for the tabs, and Esc to stop a job.
- **Clear results:** a job that had problems says so first ("Backup finished with 3 problems."), its bar turns red (yellow when stopped), and **Show what went wrong** lists every problem. A full or unplugged drive, a file in use or a wrong passphrase gets a plain message instead of an error report for the developers.
- **No freezes:** the figures that took long on large databases are read in the background after each job, and changes made in the window are saved there too.
- **Room for the warnings:** the drives line and its warnings each have a line of their own, and a click on a warning opens Next backup. Buttons are as wide as their text, also in German and Spanish, and wrap in a narrow window. Several dialogs are wider.
- **States in words:** each file in the list says its state in words as well as in colour, high contrast uses Windows' own colours, and **Help > What the file list shows** explains them.
- **Sharp on every screen:** the window follows each monitor's scaling.
- **Read back what was just written:** after a backup, a button offers to read back the new copies on that drive. It never starts by itself.
- **Shutting down:** when Windows shuts down or signs out during a job, BRODALF stops the job, saves the database, and says next time which job was cut short.
- **Smaller things:** tables sort by a click on a column header, the log keeps far more than 32,000 characters, dark tabs show which one has the focus, and BRODALF has an icon of its own.

**Installer**
- Installing or uninstalling while BRODALF or `brodalf-cli` is open asks you to close it first, instead of failing part way.

**Command line**
- `brodalf-cli` exits 0 when everything went well, 1 on an error, 2 when the command finished but files failed or copies are missing, and 3 when stopped with Ctrl+C.
- `verify --unread` checks only the copies that were never read back.

0.10.0 opens databases made by 0.9.0 and older, and 0.9.0 can still open them afterwards.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
