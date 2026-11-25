Run the installer (`brodalf-v0.9.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**Nothing new in this build has been tried on a real Windows PC yet.** It was built and tested on Linux and built and tested by Windows CI, and the new window has not been looked at; please try it on a test folder first.

This build adds the six things 0.8.0 left for later. Everything 0.8.0 settled still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens; and copies are plain files in their folders unless you choose otherwise.

**Dark mode**
- **View > Appearance**: like Windows (light or dark, as its app mode is set), Light or Dark. Dark covers the title bar, menus, tabs, lists, the tree, text boxes, buttons, the status bar and BRODALF's own dialogs. Message boxes and Windows' own question dialogs stay light. With high contrast on, BRODALF uses the high-contrast colours.

**German and Spanish**
- **View > Language**: like Windows (its display language), English, Deutsch or Español. It takes effect the next time BRODALF opens.
- The German and Spanish text is a machine translation that no native speaker has checked yet, and some longer text may be cut off in dialogs laid out for English. Corrections are welcome as an issue or a pull request (`gui/lang/`).
- Error messages from deep inside BRODALF, the command line and the installer stay in English.

**Hiding file and folder names on an encrypted drive**
- When you set up an encrypted drive or extra place, you can tick **Also hide the names of the files and folders on it**. Every file and folder on it then gets a name of 24 letters and digits, so someone holding the drive sees how many files there are and how big, but not what they are called.
- BRODALF still shows, checks and restores everything by its real name, which is kept in the database. It is chosen when the drive is set up and cannot be changed later. Encryption stays optional and off unless you choose it.

**Compressing a backup drive**
- **Compress the copies on it** (a drive's menu, or **Compress copies** on the Local backups tab) turns on Windows' own file compression for BRODALF's folder on that drive. The copies stay ordinary files that any Windows PC reads without BRODALF; they just take less room, and later backups to it are compressed too.
- Documents often shrink to half or less; photos, videos, music and zip files barely shrink. It takes about as long as reading and writing every copy, and the same menu turns it off again. It never starts by itself, and encrypted, exFAT and FAT32 drives are left out.

**Files in use, from a shadow copy**
- When a scan or backup could not read files another program has open, BRODALF offers to read them from a shadow copy: a snapshot of the drive that Windows takes and no program holds open. It is only offered, never automatic.
- On yes, Windows asks for administrator permission, BRODALF copies just those files out of the snapshot, scans them and backs them up again where the backup was going, then deletes its copies. **View > Files that were in use at the last scan** makes the same offer.

**The last database**
- BRODALF remembers the database you opened last in `%LOCALAPPDATA%\BRODALF` and offers it at start. Until a database is open, only **Open database** and **New database** work; the rest of the window stays greyed out.

**Command line**
- `brodalf-cli` gains `compress`, `drive --hide-names`, `place --hide-names`, and `scan --shadow-copy` and `backup --shadow-copy` (run as administrator).

**Left for later:** a cross-platform app (after 1.0), more languages, and translating the command line and the installer.

0.9.0 opens and upgrades databases made by 0.8.0 and older, and 0.8.0 can still open them afterwards, until a drive or extra place hides names: from then on, 0.8.0 and older cannot open that database.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
