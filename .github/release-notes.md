Run the installer (`brodalf-v0.13.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**What is new in this build has not been tried by a person on a real Windows PC yet.** It was built and tested on Linux and by Windows CI. Please try it on a test folder first.

A database is not tied to the computer it is opened on. It keeps where each backup root was last, but a backup root is its files: this build finds a root by them, and no longer takes a folder of other files for it. Everything settled before still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens.

**A root found by its files**
- When a backup root's folder is not where it was, for example under another user name or on another drive letter, BRODALF looks for its files under your user folder and on your other local drives. When one folder holds them, it carries on there and says so. When several do, or none, it asks where the root is now.
- When you are asked where a root is, the folder that holds its files is offered, with how many of the files BRODALF checked are there.

**A folder of other files at a root's path**
- A folder at a backup root's path counts as the root by what it holds: the files the database knows there, where it has them or moved inside it. A folder of other files with the same name is left as it was, and the root's files are not marked deleted, just as an empty folder was in 0.12.0. If the root's files are somewhere else, BRODALF says where.

**Wording**
- The app, the command line, the recovery sheet and the README no longer speak of a new PC. The recovery sheet says what to do when the database file is lost, on any computer.

0.13.0 opens databases made by 0.12.0 and older, and 0.11.0 and 0.10.2 can still open them afterwards: the database format is unchanged.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
