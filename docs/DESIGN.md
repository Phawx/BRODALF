# BRODALF design

Agreed with the project owner on 2026-09-28.

## What it is

BRODALF is a catalog of where your bits live. The `.brodalf` file holds no file
data. It records every file and folder you protect, every version BRODALF has
seen, and every place a copy of each version was written. The targets are
hard drives that are usually offline. (OneDrive and Dropbox storage existed
in 0.3.0 and was taken out in 0.4.0; see the README.) Windows is the main
platform and the GUI is the main way to use it. Encryption is optional, per
drive (see below).

## Flow

1. Open BRODALF. It asks for a `.brodalf` file, or creates a new one.
2. Choose the folders to protect.
3. Scan them: path, size, modified time and a BLAKE3 checksum for every file.
4. Show the ghost tree. Everything starts greyed out.
5. Plug in a drive and back up. Files light up once a correct copy is on a
   connected drive.
6. Later sessions notice drives. Plugging a known drive in reads its
   `BRODALF.media` and free space and lights up the matching files;
   unplugging greys them out. Nothing else happens until the user presses a
   button: scanning, backing up, checking and restoring are each the user's
   choice, and the user sees where each one and each drive stands.
7. BRODALF is not resident and starts nothing by itself (since 0.6.0): no
   scheduled task, no backup or check on plug-in, no reminders, no
   rescan on open. It does not care what else runs on the PC.

## Nothing automatic

Up to 0.5.0 a Windows Task Scheduler task ("BRODALF - <catalog file
name>") ran `brodalf.exe --check <catalog>` daily or weekly. 0.6.0 removes
it: when the GUI opens a catalog it deletes that task through the Task
Scheduler 2.0 COM API (`ITaskFolder::DeleteTask`, falling back to
`schtasks /Delete`), and `brodalf.exe --check <catalog>` itself only deletes
the task and exits, in case the task fires before the catalog is opened.
`--shadow-copy` exits with 1. The options "schedule", "auto_backup" and
"guard_percent" are gone; `bd_option_get` returns -1 for them.

Before a backup the GUI shows `bd_backup_preview`: what `bd_backup_ex`
would copy, move, keep under `.versions` and prune, the free space (read
now when plugged in), how much would not fit, and the oldest last scan of
the folders. It reads only the catalog and the drive's free space. It also
counts the files whose copy at its own path on the drive is good and how
many of them changed or were deleted since (moves aside); a quarter or more
of at least 50 is `mass_change`, which the GUI shows as a ransomware
warning with "keep every old version this time" ticked
(`bd_backup_opts.keep_old_versions`, no pruning).

`bd_media_status_get` says where a drive stands from the catalog alone:
live files whose current version, an older version or no version is on it
(with sizes), deleted files whose last version is on it, old versions in
`.versions`, bad and never-read-back copies, the last backup job and the
last check job. `bd_list_jobs` lists the `jobs` table (scan, backup,
restore, quick check, full check, read back; the newest 2000 are kept).
`bd_export_csv` writes every file with its drives as UTF-8 CSV with a BOM;
every text field is quoted and a leading `= + - @`, tab or CR gets a `'`
so a spreadsheet does not run it as a formula.

Free space is tracked in `space_log(media_id, at_ms, total_bytes,
free_bytes, event)`, written when a drive is plugged in and before and
after every backup to it; `media` carries the latest reading. The GUI,
`drives` and `at-risk` show it.

## Moves and renames

Versions belong to paths, so a renamed folder looks like deleted files plus
new files with the same checksums. A backup checks, for every new file it
would copy, whether a deleted node has the same hash and size with a good
copy on this drive (not under `.versions`) whose stored size and time still
match; if so the copy is renamed on the drive and the copy row re-pointed.
Folders the move empties are removed up to the catalog's folder. The
backup preview's mass-change count leaves moves out the same way.

On Windows names ignore letter case, so a path that only differs in case
from one the catalog knows (Photo.JPG that is now photo.jpg) is that same
file, renamed: the scan looks it up by `nodes.rel_fold` (the path folded
with Windows' own case mapping, indexed, and only looked at for paths the
catalog does not know exactly) and renames the node, keeping its versions
and copies. Its copy on the drive keeps the old spelling until the file
changes; the backup then finds that copy among the file's own copy rows
by the folded name. Elsewhere paths stay case sensitive and the key is
not stored.

A source's folder on a drive sits next to `.versions`, `BRODALF.media` and
`catalog-backup.brodalf`, so a source folder with one of those names (in
any case) gets a name with `_` in front (`_versions`). A catalog that
already has such a name is fixed at the next backup, which says so in the
log; copies made before keep their rows and stay good.

## Links and reparse points

The scan never follows links. On Windows only a reparse point that stands
in for another path (a symbolic link or a junction, the "name surrogate"
tags) is a link; every other reparse point is an ordinary file or folder
with extra plumbing behind it, above all OneDrive and Dropbox placeholders,
which carry the attribute even when fully downloaded. `bd_walk` reads the
tag from `WIN32_FIND_DATA.dwReserved0`; `bd_stat` asks `FindFirstFile` for
it when a path has the attribute. (Until 0.3.0 every reparse point was
skipped, which would have left a synced folder unprotected.)

## Read-back (verify) pass

Every copy row remembers when it was last fully read back (a copy written
again starts as never read back). When the user asks, `bd_media_verify`
rehashes the copies not read back in "verify_days" days (default 30; 0
every copy) and marks the bad ones damaged. A full check reads every copy;
a quick check only compares size and time. `bd_media_connect` checks
nothing; it only identifies the drive and records its free space.

## Restoring as of a time

`bd_restore_ex` with `as_of_ms` restores the newest version first seen at or
before that time, for every file known then, including ones deleted since.
(The ransomware guard of 0.3.0 to 0.5.0, which paused backups on its own
after a mass change, was taken out in 0.6.0; the backup preview warns
instead.)

## Files in use

Opening a file for reading shares read, write and delete. A sharing or lock
violation is reported through `bd_open_was_in_use()`, the scan or backup
records the path in the temp table `in_use` and leaves the catalog as it
was; `bd_list_in_use` lists them and the GUI says to close the program that
holds them and scan again. (0.5.0 read such files from a VSS shadow copy
through an elevated helper; 0.6.0 takes that out.)

OneDrive "online-only" files (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` or
`RECALL_ON_OPEN`) are not read, so they are not downloaded: the scan counts
them in `files_online_only` and leaves the file's catalog entry as it was,
and a file first seen online-only gets its first version once it is on the
PC. A backup does not copy them either.

## Progress

Long jobs report through `bd_catalog_set_progress`: phase (scan, backup,
verify, check, restore), files done and expected, bytes done and expected,
and the current path, about ten times a second; the hash loop adds bytes
as it reads. The GUI shows a bar with a percentage (bytes when the total is
known, else files, else a marquee), the counts, a data rate over the last
few seconds and the current file.

## Stopping a job

`bd_catalog_cancel` sets a flag on the catalog that any thread may set; each
job clears it when it starts and returns `BD_ERR_CANCELLED` when it saw it.
Scan, backup (moves included), prune, check, read-back and restore look at
it between files, and a copy or hash looks at it between 1 MiB blocks (the
scan's hashing threads too). Stopping keeps everything already done: a scan
drops only what it found in the source folder it was in, which reads as it
did before; a backup still syncs, renames and records the copies already
written, and a copy cut off halfway is removed, never renamed into place; a check keeps the
results so far and leaves the copy it was reading as it was; a restore
removes the file it was writing. The job is recorded as `stopped`. The GUI's
Stop button and Close (which asks first) set the flag, drop the queued jobs,
and the GUI asks again from the progress callback, so a request made between
two steps of a job is not lost when the next step clears the flag.

## Speed

- BLAKE3 is built with its SSE2, SSE4.1, AVX2 and AVX-512 code on x86-64
  (the C intrinsics files from BLAKE3 1.5.4) and picks the best the CPU has
  at run time; other targets use the portable code. `tests/test_blake3.c`
  checks the official test vectors and every path against the portable one.
- A copy longer than four 1 MiB blocks hands its blocks to a writer thread
  through a ring of three buffers (`bd_hash_copy`), so reading and hashing
  one block overlaps writing or sealing the previous one. Smaller files use
  the plain loop and start no thread.
- A scan hashes new and changed files on a few worker threads (one fewer
  than the processors, at most four, at least one), started with the first
  file that needs hashing. Results come back in walk order through a
  bounded ring of 64 slots and only the scanning thread writes the catalog,
  so it ends up exactly as a one-at-a-time scan leaves it. Workers pass
  their byte counts to the scanning thread (`bd_io_redirect`), which alone
  calls the progress callback.

## Ghost tree states

| State | Meaning |
| --- | --- |
| Available | A good copy of the current version is on connected storage |
| Older version available | The file changed since its last backup; only older versions are reachable |
| Offline | Copies exist but none of their drives is connected; the UI names a drive to plug in |
| No copy | Never backed up |
| Missing or damaged | The drive is connected but the copy is gone or fails its check |
| Deleted | Gone from the source folder; its copies are still tracked |
| Partly available | Folders only |

A version number is shown only when a file has more than one version.

## Drives

Each drive gets `BRODALF/<catalog id>/BRODALF.media`, holding a random drive
ID. Drives are recognised by that ID, not by drive letter.

Checking a copy:

- **Quick** (on every connect): the file exists with the recorded size and
  modified time. If only the time differs, it is rehashed. A copy that was
  found missing or damaged before is rehashed too, and only a matching
  hash makes it good again (0.4.0; before that an unchanged size and time
  were enough, so bit rot found by a full check was forgotten).
- **Full** (on demand): every copy is rehashed.

Copies carry the original file's modified time, and so do restored files
(the catalog's time for that version). The time recorded for a copy is
always the one read back from the drive after it was renamed into place,
so the quick check compares the drive with itself: it works the same for
copies made before this (which have the time they were written) and on
drives that round times (FAT keeps 2 seconds).

## Layout on a drive

```
X:\BRODALF\<catalog id>\BRODALF.media
X:\BRODALF\<catalog id>\<source name>\<path>              newest version on this drive
X:\BRODALF\<catalog id>\.versions\<source name>\<dir>\<name>.v<N><ext>   older versions
X:\BRODALF\<catalog id>\catalog-backup.brodalf           copy of the catalog
```

Files stay browsable without BRODALF. A new version is written to a temp file
and checked against its hash first; only then is the previous copy moved into
`.versions` and the new one renamed into place, so a failed write can never
destroy a good copy. Copies are synced to the drive in groups (up to the 256
files of a batch or about 64 MB of data): every temp file of the group is
written and checked, then the group is synced (`bd_fsync_paths`: on Windows
each file is written out without a drive cache flush and one full flush ends
the group; elsewhere one fsync each), then each copy is renamed into place
and recorded, then on POSIX the folders that got new names are synced. A
batch's catalog rows are committed in one transaction after its last group,
so no copy is recorded before its data is on the drive. A file already at the destination that the catalog does
not know about is adopted if its hash matches (for example after a crash
before the catalog was saved), otherwise it is moved aside, never overwritten.

Before copying, a backup removes old versions the keep rule no longer needs
(settings `keep_versions`, default 5, and `keep_days`, default 365: an old
version goes only when at least that many newer versions exist and it was
replaced longer ago than that; 0 keeps everything). It never writes into the
last max(16 MB, 0.5%) of the drive: a file that doesn't fit is counted as
"no room" and skipped. A follow-up backup to another drive can be limited to
files missing from up to 8 full drives (`only_missing_from`), so a large
folder spans drives. A restore plan orders the drives a restore needs,
plugged-in ones first, then by how many files each covers; restores skip
destination files that already have the right size and hash.

Scans apply a skip list (setting `skip_list`, default: temp files, system
files and rebuildable caches such as `node_modules/`). Patterns are
case-insensitive globs matched against each name, or against the path from
the protected folder when they contain a `/`; a trailing `/` means folders.

## The .brodalf file

16-byte header (`BRODALF\x1a`, format version, flags) followed by one zstd
stream of a SQLite database. While open it is unpacked to a working copy in the
temp folder. On Windows a named mutex, `Local\BRODALF-catalog-<hash>` (the
first 16 bytes of the BLAKE3 hash of the catalog's full path, made long,
upper-cased and without the `\\?\` prefix), keeps a second BRODALF on the
same PC from opening it, and `<file>.lock` is held open without sharing and
deleted on close, which also keeps out a BRODALF on another PC when the
catalog is on a share. Windows drops both when the process ends, so a crash
leaves no stale lock. On other systems (tests, CLI) `<file>.lock` is created
exclusively and removed on close. Saving
writes a new file and swaps it in atomically.

Tables: `sources`, `nodes`, `versions`, `media`, `media_hardware`, `copies`,
`space_log`, `jobs`, `settings`, `meta` (and `cloud_accounts` in catalogs
written by 0.3.0). Schema version 6: version 6 added `nodes.rel_fold`, the
case-folded path used on Windows, filled in when an older catalog is opened.

## Recovery on a new PC

Every backup leaves `catalog-backup.brodalf` in the catalog's folder on the
drive. "Recover from a backup drive..." on the start screen finds it on the
drive picked (the drive, its `BRODALF` folder or the catalog's folder), takes
the newest copy of that catalog on any plugged-in drive by file time, and
`bd_catalog_recover` copies it to a new file the user names (never over an
existing one; written as `<name>.recovering` and renamed at the end). It is
then opened as usual (passphrase asked if the copy is encrypted), and the
startup drive search quick-checks every connected drive. Protected folders
whose path is not on this PC are offered a new place first (with a guess
under `%USERPROFILE%` when the old path was under `X:\Users\<name>`);
`bd_source_relocate` changes only `sources.path`, so the folder keeps its
name on the drives and its files keep their versions and copies. A scan
skips an unreachable folder rather than marking its files deleted, so a
folder left for later is safe. Catalog > "Change where a folder is..." does
the same at any time.

## Protection target

The catalog's settings hold a target of N copies in M places (default 2 and
2). For every live file, the current version's good copies are counted by
distinct medium, and places by distinct place key: a drive's location
(trimmed, ignoring case; empty for all drives with none set; a cloud account
left by 0.3.0 keeps a key of its own). A file is at risk when copies < N or
places < M. A file is "changed since its last backup" when its current
version has no good copy anywhere but an older version has. For "which
drive next", a medium helps a file at risk when it has no good copy of the
current version and either the file needs copies or the medium's place is
not yet among the file's places. Offline copies count: the target is about
what exists, not what is plugged in.

## Replacing a drive

`media.retired_ms` and `media.replaced_by` (added in place to schema 5
catalogs; earlier versions ignore them) mark a drive being replaced and the
drive taking its place. A retired drive gets nothing new: `bd_backup`
refuses it, "which drive next" never names it, and the GUI leaves it out of
plug-in backups. Its copies still restore. A copy on it counts toward the
protection target (`BD_SQL_COUNTS`) until the replacement holds a good copy
of the same version, so the target is met without it once the replacement
has caught up. `bd_media_replace(old, new)` retires old, lists in
`temp.replace_need` every current version with a good copy on old that has
fewer than N copies or M places on the other drives, and runs a backup to
new limited to that list (read from the source folders and checked by hash,
like any backup). Deleted files and older versions whose only copy is on
old are not copied; they are counted so the GUI can say to keep the drive.
The drive details show a plain suggestion to replace a drive whose health
data says failing, warning, 90% worn, or pending or unreadable sectors.

## Deleted files

A file removed from its protected folder keeps its node (`deleted<>0`,
`last_seen_ms` the last scan that found it) and its versions and copies.
`bd_list_deleted` lists the ones whose current version has a good copy,
most recently seen first, with the drives that hold it. Restoring with
`bd_restore_opts.deleted_only` (bound as `?5` in `RESTORE_VERSION` and
`RESTORE_WHERE`) brings back only deleted files under the given file or
folder, each as it was last seen. The GUI's Show deleted files lists up to
5000 of them; it restores the selected files, or everything deleted from
the folders they were in, in one restore job. Files on drives that are not
plugged in are reported, not resumed when the drive arrives: the user
restores them again from the list.

## Drive hardware

`media.location` holds the optional "where it is kept" text. `media_hardware`
holds the last reading of each drive, taken whenever it is plugged in
(`src/drive_hw.c`): on Windows the volume is mapped to its physical disk
(`IOCTL_STORAGE_GET_DEVICE_NUMBER`), then `IOCTL_STORAGE_QUERY_PROPERTY` gives
vendor, model, firmware, serial and bus; `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`
the size; the temperature property (`StorageDeviceTemperatureProperty`, 52)
the temperature; `IOCTL_STORAGE_PREDICT_FAILURE` the failure prediction and,
for ATA and SATA disks only, the ATA SMART attribute table (ids 5, 9, 12,
190/194, 197, 198, 231; other buses put something else in those bytes); for
NVMe disks only the NVMe health log page through the protocol-specific query; and, as a fallback that needs
administrator rights, `SMART_RCV_DRIVE_DATA`. Health is "failing" when the
drive predicts failure, "warning" with any reallocated, pending or
unreadable sectors, NVMe critical-warning bits or 100% wear, else "good".
The drive's identity stays the `BRODALF.media` UUID; a serial number that
changes under the same UUID is logged.

## Cloud (removed in 0.4.0)

OneDrive and Dropbox storage was built in 0.3.0 and taken out in 0.4.0; the
code is in git history at tag `v0.3.0`. What remains: `media.kind` can still
be `onedrive` or `dropbox` in a catalog written by 0.3.0 (such a row is never
connected, is shown as unsupported, and its copies count as a place of their
own), the empty `cloud_accounts` table in those catalogs, and the unused
`copies.stored_rev` column. Storage still goes through the one interface in
`src/store.h` (stat, local path, download, staging path, upload, move,
remove, space), which local drives implement with plain file operations.

## Encryption

Optional, per drive, and it encrypts file contents only: names and folders
stay readable so a drive can still be browsed (Phawx's call, 2026-09-28).
Built on [Monocypher](https://monocypher.org) 4.0.2, two vendored files.

- A catalog can have one passphrase. It protects a random 32-byte master key:
  Argon2id (64 MiB, 3 passes) turns the passphrase into a key that wraps the
  master key with XChaCha20-Poly1305. The wrapped key (the 96-byte "key
  block": salt, cost, nonce, MAC, wrapped key) is kept in `settings`. A wrong
  passphrase fails to unwrap. Changing the passphrase rewraps the same master
  key, so nothing on the drives needs rewriting.
- A drive set up as encrypted (`media.encrypted`, and `encrypted=1` in
  `BRODALF.media`) stores every copy as `<name>.bdenc`. Older versions in
  `.versions` become `<stem>.v<N><ext>.bdenc`. This can't be changed later.
- An encrypted file is a 40-byte header (`BRDLFENC`, version, chunk size,
  random 24-byte nonce), then 64 KiB chunks, each with a 16-byte MAC. Every
  chunk is authenticated with the header and a "last chunk" flag, and the
  stream rekeys after each chunk, so tampering, reordering and truncation all
  show up as a damaged copy.
- Hashes in the catalog are always of the plaintext, so a copy on an
  encrypted drive and one on a plain drive are the same version.
- The key is needed to write, full-check or restore an encrypted copy, not to
  see it in the tree. A quick check works without it (size and time), and a
  copy that was touched since it was written is left as it was until a check
  with the passphrase.
- The `.brodalf` file can be encrypted too (flag bit 0 in the header). The
  header is then followed by the key block, and the zstd stream is sealed with
  the master key in the same chunk format. The catalog backup on an encrypted
  drive is always written encrypted.
- The master key, salts and nonces come from the system's random source
  (BCryptGenRandom, getrandom). If it fails, nothing is made or written: no
  key block is stored, the old one stays, a sealed copy or catalog file is
  not started, so a predictable key or a repeated nonce can never happen.
- A key block's Argon2 settings are checked before any work: memory 8 MiB
  to 1 GiB and 1 to 16 passes (BRODALF writes 64 MiB and 3; it always uses
  one lane, which the block does not store). Anything else is refused as a
  damaged catalog instead of trying to allocate 4 GiB or run for hours.
- A recovery key is a second way to unlock: 30 characters from
  `23456789ABCDEFGHJKLMNPQRSTUVWXYZ` (no 0, 1, I or O; 150 bits) in groups of
  five, made only when asked for (the recovery sheet) and shown once. It wraps
  the same master key the same way as the passphrase (Argon2id with its own
  salt, then XChaCha20-Poly1305); the block is kept as `recovery_block` in
  `settings`, and an encrypted `.brodalf` file also carries it in the header
  after the key block, with flag bit 1 set, so it opens a catalog backup on a
  new PC. BRODALF before this change refuses such a file as a newer format.
  Typed keys ignore case, dashes and spaces. A new recovery key replaces the
  old one; changing the passphrase keeps it.

## Recovery sheet

`bd_write_recovery_sheet` writes one self-contained HTML page (no scripts,
images or links; every catalog string HTML-escaped): what to do on a new PC,
each drive with where it is kept, size, free space as last seen, the
protected folders it holds current versions of with file counts and sizes,
and its last backup (from `jobs`), the protected folders, and the recovery
key when one was just made. The passphrase is never written. The GUI saves
it where the user chooses, opens it in the browser for printing and says to
delete the file afterwards; making a key also refreshes the catalog backup
on every plugged-in drive so they know the new key.

## GUI

Native Win32 in `gui/main.c`, with comctl32 v6 and a DPI-aware manifest.

- Menu bar: Catalog (folders, scan, files in use, restore, restore as of a
  date, deleted files, recovery sheet, CSV export, exit), Local backups (each
  known disk with its own submenu: back up, quick check, read back, full
  check, where it stands; add a disk; what needs backing up; all disks;
  status of every disk; history), Help (log, reports, about). The Local backups menu is filled on
  `WM_INITMENUPOPUP`.
- A progress bar and a text line sit above the log; see Progress above.
- Startup: a task dialog offers the last catalog (kept in
  `HKCU\Software\BRODALF\LastCatalog`), another one, or a new one. A path on
  the command line opens directly. A catalog open in another BRODALF window
  is refused with a message naming it.
- The tree loads folders lazily and draws each state with custom draw: grey
  italic for no reachable copy, amber for older version only, red bold for a
  bad copy, strikethrough for deleted. Folder and source rows show "x of y
  available" and, when offline, the drive that holds the copies.
- The detail panel lists every version and each copy: drive, state, last
  checked, size and path on the drive.
- Scan, backup, check and restore run one at a time on a worker thread, which
  reports progress to the status bar and log and saves the catalog after each
  job. The tree is refreshed when a job ends and cannot be expanded while one
  runs, since the catalog belongs to the worker then. The reverse holds too:
  while the UI thread is in a command or in what follows a job (its
  end-of-job prompts, which may wait in a message box), no job starts; a
  drive plugged in meanwhile is queued and handled when the UI is done.
  The search box says search is available when the current job finishes,
  and searches again then.
- Drives: on startup and on `WM_DEVICECHANGE`, drives that disappeared are
  disconnected, every drive letter with a `BRODALF.media` for this catalog is
  connected (read only: its identity and free space), and folders or shares
  used as storage are retried where they were last seen. The log says which
  drive was plugged in and, where it applies, that it is due for a full
  check, holds files for a waiting restore, or can take what did not fit on
  a full drive. Nothing is started.
- Back up shows the preview first (Start, Scan first then back up, or
  Cancel). Check drive offers a quick check, a full check or a read back. A
  restore that waits for drives continues when the user presses Restore...
  and picks Continue.

## Installer

`installer/brodalf.nsi` (NSIS 3, `makensis -DVERSION=x.y.z`) installs
brodalf.exe, brodalf-cli.exe and THIRD-PARTY-NOTICES.txt for the current
user into `%LOCALAPPDATA%\Programs\BRODALF` with no administrator rights
(`RequestExecutionLevel user`), adds a Start menu shortcut and an uninstall
entry under `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\BRODALF`.
The uninstaller removes only those files, the shortcut and that key: catalogs,
the app log, `HKCU\Software\BRODALF` and any scheduled check task left by
0.5.0 or earlier are left,
since they belong to the user's catalogs. The installer is not signed.
