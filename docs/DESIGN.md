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
6. Later sessions rescan and watch for drives. Plugging a known drive in checks
   its copies, reads back the ones not read back lately, and lights up the
   matching files; unplugging greys them out.
7. BRODALF is not resident. A scheduled task runs it once a day without a
   window to rescan and say what needs backing up and which known drive has
   room for it.

## Running on a schedule

The app never runs in the background. `brodalf.exe --check <catalog>` is
what Windows Task Scheduler runs (task "BRODALF - <catalog name>", daily or
weekly at 12:00, from the "schedule" option: 1, 7 or 0; the GUI registers
the task on every start when the option is on, so a new catalog gets a
daily task straight away). It scans silently,
and only when files fall short of the protection target (or the guard
tripped) shows one message: how much is at risk, and the drive to plug in,
with an "Open BRODALF now?" button. The drive is `bd_suggest_drive`: the
first known drive that would take everything at risk and had room for it
when last seen, else the one with the most free space.

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
Folders the move empties are removed up to the catalog's folder. The scan
keeps a temp table of nodes that are new or came back in this scan, so the
guard can subtract moves from what looks like damage.

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

Every copy row remembers when it was last fully read back. When a drive
connects, `bd_media_verify` rehashes the copies not read back in
"verify_days" days (default 30) and marks the bad ones damaged. A full
check reads every copy; a quick check only compares size and time.

## Ransomware guard

After a scan, if changed plus deleted files, minus moves, reach
"guard_percent" (default 25) of the files known before the scan, and at
least 50 files were known, the guard trips. Settings `guard.tripped_ms`,
`guard.before_ms` (the previous scan's time), `guard.changed`,
`guard.deleted` and `guard.total` are written once, on the first trip, so
later scans do not move the "before" point. While tripped, `bd_backup`
returns `BD_ERR_GUARD` (unless `ignore_guard`) and version pruning is
skipped. `bd_restore_ex` with `as_of_ms` restores the newest version first
seen at or before that time, for every file known then including ones
deleted since; "before the changes" is `as_of_ms = guard.before_ms`.
`bd_guard_clear` resumes.

## Files in use

Opening a file for reading shares read, write and delete. A sharing or lock
violation is reported through `bd_open_was_in_use()`, the scan records the
path in the temp table `in_use` and leaves the catalog as it was. The GUI
offers a shadow copy: `brodalf.exe --shadow-copy <dir> <list>` runs
elevated (`src/shadow.c`, the VSS backup-components API on `vssapi.dll`),
snapshots each volume involved, copies the listed files out of the snapshot
into `<dir>` and writes `result.txt`. The GUI then registers each staged
file as a substitute for its path (temp table `substitute`), rescans and
backs up; the scan hashes the substitute and the backup copies it, so the
catalog records the real path.

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
temp folder and `<file>.lock` prevents a second window from editing it. Saving
writes a new file and swaps it in atomically.

Tables: `sources`, `nodes`, `versions`, `media`, `media_hardware`, `copies`,
`space_log`, `jobs`, `settings`, `meta` (and `cloud_accounts` in catalogs
written by 0.3.0). Schema version 6: version 6 added `nodes.rel_fold`, the
case-folded path used on Windows, filled in when an older catalog is opened.

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

## Drive hardware

`media.location` holds the optional "where it is kept" text. `media_hardware`
holds the last reading of each drive, taken whenever it is plugged in
(`src/drive_hw.c`): on Windows the volume is mapped to its physical disk
(`IOCTL_STORAGE_GET_DEVICE_NUMBER`), then `IOCTL_STORAGE_QUERY_PROPERTY` gives
vendor, model, firmware, serial and bus; `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`
the size; the temperature property the temperature;
`IOCTL_STORAGE_PREDICT_FAILURE` the failure prediction and the ATA SMART
attribute table (ids 5, 9, 12, 190/194, 197, 198, 231); the NVMe health log
page through the protocol-specific query; and, as a fallback that needs
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

## GUI

Native Win32 in `gui/main.c`, with comctl32 v6 and a DPI-aware manifest.

- Menu bar: Catalog (folders, scan, restore, restore as of a date, exit),
  Local backups (each known disk with its own submenu: back up, read back,
  full check, details; add a disk; what needs backing up), Help (shadow
  copies, log, reports, about). The Local backups menu is filled on
  `WM_INITMENUPOPUP`.
- A progress bar and a text line sit above the log; see Progress above.
- Startup: a task dialog offers the last catalog (kept in
  `HKCU\Software\BRODALF\LastCatalog`), another one, or a new one. A path on
  the command line opens directly. A stale lock can be removed after a crash.
- The tree loads folders lazily and draws each state with custom draw: grey
  italic for no reachable copy, amber for older version only, red bold for a
  bad copy, strikethrough for deleted. Folder and source rows show "x of y
  available" and, when offline, the drive that holds the copies.
- The detail panel lists every version and each copy: drive, state, last
  checked, size and path on the drive.
- Scan, backup, check and restore run one at a time on a worker thread, which
  reports progress to the status bar and log and saves the catalog after each
  job. The tree is refreshed when a job ends and cannot be expanded while one
  runs, since the catalog belongs to the worker then.
- `brodalf.exe --shadow-copy` is dispatched before `CoInitializeEx`: the
  helper needs the multithreaded apartment, and VSS hangs in a
  single-threaded one.
- Drives: on startup and on `WM_DEVICECHANGE`, drives that disappeared are
  disconnected, every drive letter with a `BRODALF.media` for this catalog is
  connected (with a quick check), and folders or shares used as storage are
  retried where they were last seen.
