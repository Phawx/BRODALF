Run the installer (`brodalf-v0.12.0-windows-x64-setup.exe`): it needs no administrator rights, adds BRODALF to the Start menu and, if you tick it, the desktop. Or unzip the zip anywhere and run `brodalf.exe`.

**Windows SmartScreen** will warn that the app is from an unknown publisher, because it is not code-signed. Click **More info**, then **Run anyway**.

**What is new in this build has not been tried by a person on a real Windows PC yet.** It was built and tested on Linux and by Windows CI. Please try it on a test folder first.

This build fixes 21 gaps found by walking real use cases through 0.11.0, from first setup to a new PC. Everything settled before still holds: nothing on a backup drive is ever overwritten, moved or deleted; nothing starts by itself except the scan when a database opens.

**A new PC, or a reinstalled Windows**
- Windows makes empty Documents, Pictures and Desktop folders under the old names. A backup root that now holds a tenth or less of the files it had is no longer marked deleted: it is held as it was, and BRODALF offers to restore it, to say where it is now, or, if the files really were deleted, to mark them so after asking.
- A backup root on a disk that is not plugged in can wait for it: **Skip it until the disk is plugged in**.
- A drive holding backups from another database (your old PC's) offers to recover that database. **Recover** tries an older copy when the newest is damaged and says which one opened, and opening a database copy that is on a backup drive offers Recover instead, so the drive's copy is never saved over.
- The recovery sheet names this version and the releases page, and says how to copy files off a plain drive without BRODALF.

**Copies that count**
- Copies now count once per physical disk, so two partitions of one disk are one copy. A drive on the same disk as your files, and one whose health data says it is failing, no longer count. A drive's details say why.
- A drive that was reformatted or erased is recognized by its model, serial number and size: **Is this Blue, wiped?** marks its old copies missing (after asking twice) and backs up to it again under its name.
- The warning that many files changed at once, which can mean ransomware, now comes up before the first backup to a new or almost empty drive too.

**Nothing left out without a word**
- A OneDrive or Dropbox file that is online-only here and was changed on another device counts as at risk, since its newest version is not backed up. Files never on this PC say to set them to **Always keep on this device**.
- Links (junctions, symbolic links) that lead outside your backup roots are counted after a scan, and BRODALF offers to back up the folders they lead to. Choosing a link as a backup root offers the folder it leads to.
- A file that changes during every copy, like a virtual machine disk, goes on the list of files in use, so the shadow-copy offer comes up for it.
- A file bigger than every drive you have says so, and BRODALF offers to leave it out.

**New views and checks**
- **View > Older versions using the most space**: the files whose old copies take the most room on your drives. From it, or from **Largest files**, **Leave it out...** stops backing a file up. Nothing is deleted.
- A restore checks the free space first and says if it will not fit.
- **Check drive** reads the newest database copy on the drive, and lists half-written `.brodalf-tmp` files from a copy that stopped (for example at a power cut), which it removes only if you say so.
- Changing the passphrase or the recovery key says that the older database copies on your drives still open with the old one.
- The database is left out of a backup root it is saved in, with its side files, and BRODALF says so.
- The file list's Status column says **Not on this PC** for screen readers, which the fading alone did not.
- A line in What to leave out that starts with `/` is a path from the top of the backup root.

0.12.0 opens databases made by 0.11.0 and older, and 0.11.0 and 0.10.2 can still open them afterwards: the database format is unchanged. One difference: a What to leave out line like `/huge.bin`, with no other `/`, is read by 0.11.0 and older as that name anywhere in the root.

`brodalf-cli.exe` is the command-line version; run it with no arguments for help.
