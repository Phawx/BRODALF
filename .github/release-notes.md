Run the installer (`brodalf-v0.10.2-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**What is new in this build has not been tried by a person on a real Windows PC yet.** It was built and tested on Linux and by Windows CI. Please try it on a test folder first.

This build closes one gap in encryption. It adds no new features and changes nothing in the window. Everything settled before still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens.

**An encrypted database stays encrypted on the disk**
- Until now, while an encrypted database was open, BRODALF kept an unencrypted working copy of it in the temp folder, and a crash or a power cut could leave that copy behind. Now an encrypted database is kept in memory while it is open and saved straight from memory, so nothing of it is written to the disk unencrypted.
- Turning on **Encrypt the database file** moves the open database into memory at once and deletes its working copy. A database that is not encrypted keeps its working copy as before.
- The passphrase's key is kept out of Windows' page file, and Windows' crash reports leave out the memory the database is in.
- Windows can still write memory to its page file, or to `hiberfil.sys` when the PC hibernates. BitLocker on the system drive covers both.
- An encrypted database takes about its own size in memory, and twice that for a moment while it is saved.

0.10.2 opens databases made by 0.10.1 and older, and 0.10.1 can still open them afterwards: the database format is unchanged.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
