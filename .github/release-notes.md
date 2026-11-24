Run the installer (`brodalf-v0.5.0-windows-x64-setup.exe`): it needs no administrator rights and adds BRODALF to the Start menu. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**Nothing new in this build has been tried on a real Windows PC yet.** It was built and tested on Linux and built by Windows CI; please try it on a test folder first.

New in this build:
- **Faster.** Checksums use your processor's fast instructions (the checksums themselves are unchanged), copies read the next part of a file while writing the last, a scan checks several files at once, and backing up many small files waits for the disk far less often.
- **Stop** a running scan, backup, check or restore. What was done until then is kept. Closing BRODALF during a job asks whether to stop it.
- **Recover from a backup drive…** on the start screen: on a new PC, start from the catalog copy on any backup drive, then point folders that moved at their new place.
- **Replace this drive…** in Drives: retire a drive and fill a new one with what would be left short without it. BRODALF suggests it when a drive's health says it is failing or worn.
- **Print a recovery sheet…**: a page to print with your drives, where each is kept and what is on it, and, for encrypted catalogs, a recovery key that unlocks the catalog if you forget the passphrase.
- **Show deleted files…**: files removed from your folders that are still on your drives, with restore.
- **An installer**, and the download now includes the licences of the libraries BRODALF uses.
- The scheduled check now also runs when a laptop is on battery.

Fixed:
- Copies and restored files keep the original file's modified time.
- On Windows, renaming Photo.JPG to photo.jpg is seen as a rename, not a delete and a new file. A folder named `.versions` can no longer get mixed up with old versions on the drive.
- Two copies of BRODALF can never open the same catalog, and the question about a stale lock after a crash is gone.
- Plugging in a drive while BRODALF is asking you something no longer starts a backup underneath it, and search waits until a job finishes.
- Reading files that are in use is safer: the helper that runs as administrator only reads files inside your protected folders, from a list only administrators can change.
- Drive health shows the right temperature, and NVMe and USB-attached disks no longer show made-up SMART values.
- Encryption fails safely if Windows cannot supply random numbers, and a damaged catalog's key settings are refused instead of used.

An encrypted catalog that has a recovery key cannot be opened by 0.4.0 or earlier.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
