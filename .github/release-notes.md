Run the installer (`brodalf-v0.8.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**Nothing new in this build has been tried on a real Windows PC yet.** It was built and tested on Linux and built by Windows CI, and the new window has not been looked at; please try it on a test folder first.

This build adds four things picked from what 0.7.0 left for later. Everything 0.7.0 settled still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens; and copies are plain files in their folders.

**Tabs**
- The window now has six tabs: **Primary Database** (the tree, search and details), **Next backup**, **Local backups**, **Starred files and extra places**, **Encryption** and **Restore**. Each has buttons for everything on it, and BRODALF opens on the tab you used last.
- **Next backup** lists the files the next backup copies to a drive, an extra place or any drive, with a column for each full copy saying whether the file has it yet.
- **Local backups** lists every drive with its size, free space, BRODALF's copies, other files, the space kept free, its health (SMART, temperature, hours on, wear), its oldest read-back and its problems.
- **Restore** restores what is selected, everything or the deleted files, and lists the files the last restore left alone beside the backed-up copy's size and date.
- The menus are now **File**, **View**, **Tools** and **Help**.

**Starred files and extra places**
- Star the files and folders that matter most. They are backed up to your drives as before, and also to your **extra places**: a secondary backup in any folder you name, such as your Dropbox, OneDrive or Google Drive folder, a network share or a folder on another disk.
- An extra place needs a description good enough to tell it apart (at least two words), can have a size limit, and can be encrypted. Only starred files go there, as plain files in their folders.
- BRODALF only copies files into the folder; that folder's own app does any syncing. Copies there can be restored but never count toward the copies you want.
- A scan leaves out BRODALF's own copies when an extra place is inside a backup root.

**Encrypting only some folders**
- Encryption stays optional and off unless you choose it. You can now encrypt chosen files and folders on every drive, as well as whole drives and the database file.
- The **Encryption** tab measures how fast this PC encrypts and says how much time that adds, for everything you chose and for each folder. On a USB hard drive a backup takes about as long as before; checking and restoring an encrypted copy take about the same again.
- Copies made before you chose a folder stay as they are, since nothing on a drive is overwritten; new copies and new versions are encrypted.

**Backing up to several drives at once**
- **Back up to every plugged-in drive** backs up to each one after another. Tick **Back up to several drives at once** (Local backups tab or Settings) and BRODALF reads each file once and writes it to all of them at the same time.
- That is quicker, but the PC works harder while it runs: other programs may be slower, and the backup goes at the pace of the slowest drive. It is off until you turn it on.

**Command line**
- `brodalf-cli` gains `star`, `unstar`, `stars`, `place`, `place-limit`, `places`, `encrypt-folder`, `plain-folder`, `encrypted-folders` and `pending`, and `backup` takes several drives (with `--parallel` to write to them at once).

**Left for later:** dark mode, translations, a cross-platform app (after 1.0), hiding file and folder names on a drive, a compression helper and reading files in use from a shadow copy.

0.8.0 opens and upgrades databases made by 0.7.0 and older. After that, 0.7.0 cannot open the database.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
