Run the installer (`brodalf-v0.14.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**What is new in this build has not been tried by a person on a real Windows PC yet.** It was built and tested on Linux and by Windows CI. Please try it on a test folder first.

This build takes any set of folders as backup roots, from any drive or network share. Everything settled before still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens.

**Folders typed from the current folder**
- A folder given as `Documents` or `..\Photos` is kept as its full path, in the app, the command line and when a root is moved, so a scan run from any other folder finds it. A root that 0.13.0 kept as typed is found by its files and given its full path; nothing is marked deleted.

**Any number of folders at once**
- `brodalf-cli new` and `brodalf-cli add` take any number of folders in one command. 0.13.0 dropped those past the 20th without a word. Each folder added is named with its full path, and one that is already a backup root says so.

**One folder reached two ways**
- A mapped network drive letter and its `\\server\share` path count as one backup root, and so do a folder reached through a link and a `subst` drive letter and the folder it stands for. Adding it the other way says it is already backed up, in the app and the command line.
- A folder inside a backup root, or holding one, is refused whichever way it is reached, and a database kept inside a root reached another way is still left out of it.
- Two names for one place that only the network could tell apart, like `\\localhost\C$` and `C:\`, still count as two folders.

0.14.0 opens databases made by 0.13.0 and older, and 0.11.0 and 0.10.2 can still open them afterwards: the database format is unchanged.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
