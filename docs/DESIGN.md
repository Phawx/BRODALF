# BRODALF design

Agreed with the project owner on 2026-09-28.

## What it is

BRODALF is a catalog of where your bits live. The `.brodalf` file holds no file
data. It records every file and folder you protect, every version BRODALF has
seen, and every place a copy of each version was written. The main targets are
hard drives that are usually offline; OneDrive and Dropbox come later as
optional hooks. Windows is the main platform and the GUI is the main way to use
it. Encryption is optional, per drive (see below).

## Flow

1. Open BRODALF. It asks for a `.brodalf` file, or creates a new one.
2. Choose the folders to protect.
3. Scan them: path, size, modified time and a BLAKE3 checksum for every file.
4. Show the ghost tree. Everything starts greyed out.
5. Plug in a drive and back up. Files light up once a correct copy is on a
   connected drive.
6. Later sessions rescan and watch for drives. Plugging a known drive in checks
   its copies and lights up the matching files; unplugging greys them out.

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
  modified time. If only the time differs, it is rehashed.
- **Full** (on demand): every copy is rehashed.

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
destroy a good copy. A file already at the destination that the catalog does
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

Tables: `sources`, `nodes`, `versions`, `media`, `cloud_accounts`, `copies`,
`jobs`, `settings`, `meta`.

## Protection target

The catalog's settings hold a target of N copies in M places (default 2 and
2). For every live file, the current version's good copies are counted by
distinct medium, and places by distinct place key: a drive's location
(trimmed, ignoring case; empty for all drives with none set) or one key per
cloud account. A file is at risk when copies < N or places < M. For "which
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
changes under the same UUID is logged. Schema version 4.

## Cloud

OneDrive and Dropbox, each in its app folder (`Apps/BRODALF`), so BRODALF can
see nothing else in the account. A cloud account is a row in `media` (kind
`onedrive` or `dropbox`) plus one in `cloud_accounts` (provider, account name,
root path, and `credential_ref`, the name of the saved sign-in). Files go to
`BRODALF/<catalog uuid>/...` with the same layout as a drive, including
`BRODALF.media`, `.versions` and `catalog-backup.brodalf`.

- Sign-in: OAuth 2 authorization code with PKCE (S256) through the system
  browser, redirected to a one-shot listener on `http://localhost:53682/`. No
  client secret. The refresh token goes to Windows Credential Manager
  (`BRODALF/cloud-<media uuid>`, split into 2 KB parts if needed); Microsoft
  rotates it on every refresh and BRODALF saves the new one each time.
- Signing in to an account that is already storage for the catalog signs it
  in again instead of adding it twice.
- All storage goes through one interface (`src/store.h`): stat, download,
  upload, move, remove, space. Backup uploads to `<name>.brodalf-tmp`, moves
  the old copy into `.versions`, then moves the new one into place.
- Uploads: OneDrive up to 4 MiB in one PUT, larger through an upload session
  in 10 MiB chunks; Dropbox up to 8 MiB in one call, larger through an upload
  session in 8 MiB chunks.
- Each copy records the provider's content hash (`quickXorHash`, Dropbox
  `content_hash`) in `copies.stored_rev`. A quick check compares size and
  hash; a full check downloads and verifies BLAKE3.
- Expired access tokens are refreshed once on a 401; 429 and 5xx wait for
  Retry-After (or back off) and retry.
- Every catalog save also uploads `catalog-backup.brodalf` to each connected
  cloud account (Phawx, 2026-09-28). A failure there is logged, not fatal.
- A local synced OneDrive or Dropbox folder can still be used as a plain
  drive.

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

## GUI

Native Win32 in `gui/main.c`, with comctl32 v6 and a DPI-aware manifest.

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
- Drives: on startup and on `WM_DEVICECHANGE`, drives that disappeared are
  disconnected, every drive letter with a `BRODALF.media` for this catalog is
  connected (with a quick check), and folders or shares used as storage are
  retried where they were last seen.
