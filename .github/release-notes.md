Run the installer (`brodalf-v0.11.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**What is new in this build has not been tried by a person on a real Windows PC yet.** It was built and tested on Linux and by Windows CI, and looked at in automated screenshots in English, German and Spanish, light and dark. Please try it on a test folder first.

This build redraws the window. Everything settled before still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens.

**The Primary Database tab looks like Windows Explorer**
- The folders of your backup roots on the left, the files and folders in the one you pick in the middle, and the details of what is selected on the right. Double-click a folder to open it; Backspace goes up.
- The file list has the columns Name, Status, Copies on, Date modified, Type and Size. **Copies on** names the drives that hold a copy, in grey for those not plugged in.
- The details read like a form, with one row for each drive that has a copy, and say which drive to plug in to get a file back.

**Files that are not here are drawn faded**
- A file or folder that is not on this PC and that no plugged-in drive holds is drawn faded with a grey name, the way Explorer shows a file kept offline. A backup root that is not on this PC has a red cross on its icon.
- So a database opened on a PC with none of its files still shows every file and folder, with the drives each one is on. Plug a drive in and what it holds comes back to full colour.
- **Help > What the file list shows** draws a sample of each state, with what it means.

**A clearer window**
- The top says at a glance whether every file has its copies, with a card for each drive and extra place.
- Local backups groups the drives by plugged in, not plugged in, and lost or broken. Settings is one window. Next backup, Restore, Search, Files at risk and the Encryption tab say plainly what they hold, and the Back up and Check drive menus say what each choice does.
- An error when a database opens says what went wrong and offers the way out.
- Dark mode now covers tick boxes and choices. In a low window the log starts closed, and a strip under the tabs sizes it.

**The log in your language**
- BRODALF's own log lines now show in German and Spanish too, with paths and names as they are. Error details from SQLite and the database itself stay in English.

0.11.0 opens databases made by 0.10.2 and older, and 0.10.2 can still open them afterwards: the database format is unchanged.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
