# AFS — The Afriyie File System

**Status:** 📐 designed · 📐 implemented at v0.9

The native on-disk format. FAT32 gets us booting; AFS is what the system actually
runs on, and it exists because we need three things FAT32 cannot give us:
crash consistency, real permissions, and file sizes and counts that do not
depend on 1980s design constraints.

---

## 1. Design goals

| Goal | How |
| --- | --- |
| **Crash consistency** | A metadata journal, replayed at mount |
| **Simple enough to implement correctly** | Extents rather than block maps; a bitmap allocator; no B-trees at v0.9 |
| **Fast sequential I/O** | Extents keep a file's blocks contiguous |
| **Understandable on-disk** | A `mkafs.py` writer and a host-side `afriyie-fsck`, both in Python, both readable |
| **Portable** | Little-endian fixed-width fields, no native struct layout on disk |

Everything on disk is written field by field at explicit offsets, never by
`memcpy`-ing a C struct. Structure layout is a compiler detail; the disk format
is a specification.

---

## 2. Volume layout

```
┌───────────────────────────────────────────────────────────────┐
│ Block 0                 superblock                            │
│ Blocks 1..J             journal    (default 4 MiB = 1024 blocks)│
│ Blocks S..S+B-1         inode table (fixed size, set at format)│
│ Blocks ...              block bitmap (one bit per block)      │
│ Blocks ...              data blocks                            │
└───────────────────────────────────────────────────────────────┘
```

Block size is 4 KiB, matching `AF_PAGE_SIZE`, so a file read maps directly onto
page cache frames with no repacking.

---

## 3. Superblock (block 0)

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0x00 | 8 | `magic` | `"AFS1\0\0\0\0"` |
| 0x08 | 4 | `version` | 1 |
| 0x0C | 4 | `block_size` | 4096 |
| 0x10 | 8 | `block_count` | Total blocks in the volume |
| 0x18 | 8 | `inode_count` | Fixed at format time |
| 0x20 | 8 | `inode_table_block` | First block of the inode table |
| 0x28 | 8 | `bitmap_block` | First block of the block bitmap |
| 0x30 | 8 | `bitmap_blocks` | Blocks the bitmap occupies |
| 0x38 | 8 | `journal_block` | First block of the journal |
| 0x40 | 8 | `journal_blocks` | Journal length |
| 0x48 | 8 | `root_inode` | Always 1 |
| 0x50 | 8 | `free_blocks` | Hint; the bitmap is authoritative |
| 0x58 | 8 | `free_inodes` | Hint |
| 0x60 | 8 | `mount_count` | Incremented on every read-write mount |
| 0x68 | 8 | `last_mount_ns` | |
| 0x70 | 8 | `seq` | Monotonic transaction sequence |
| 0x78 | 4 | `flags` | `AFS_FLAG_CLEAN` / `AFS_FLAG_NEEDS_REPLAY` |
| 0x7C | 4 | `checksum` | CRC32 of bytes 0x00..0x7C |

The `flags` field is the crash signal: a clean unmount writes `CLEAN`, and a
mount finds it clear only when the system died mid-write. That is what triggers
journal replay.

---

## 4. Inodes

Fixed 256 bytes each, addressed as `inode_table_block * block_size + inode * 256`.
Inode 0 is reserved (invalid), inode 1 is the root directory.

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0x00 | 2 | `type` | 0 free, 1 file, 2 directory, 3 symlink, 4 device |
| 0x02 | 2 | `mode` | POSIX permission bits |
| 0x04 | 2 | `link_count` | |
| 0x06 | 2 | `extent_count` | |
| 0x08 | 8 | `size` | Bytes |
| 0x10 | 8 | `allocated_blocks` | For a sparse-file-aware stat |
| 0x18 | 8 | `created_ns` | |
| 0x20 | 8 | `modified_ns` | |
| 0x28 | 8 | `accessed_ns` | |
| 0x30 | 4 | `uid` | |
| 0x34 | 4 | `gid` | |
| 0x38 | 4 | `checksum` | CRC32 of bytes 0x00..0x38 and 0x40..0xF8 |
| 0x3C | 4 | `_reserved` | |
| 0x40 | 184 | `extents[23]` | 8 bytes each |
| 0xF8 | 8 | `indirect_block` | Block of further extents, 0 if unused |

### 4.1 Extents, not block maps

```c
typedef struct {
    af_u32 block;   /* first physical block          */
    af_u16 count;   /* number of contiguous blocks   */
    af_u16 _pad;
} afs_extent_t;     /* 8 bytes */
```

23 inline extents × 65535 blocks each, plus an indirect block holding 512 more
extents, addresses a maximum file size in the hundreds of gigabytes with a
metadata structure small enough to reason about completely.

A block map would be simpler to allocate but turns a large sequential read into
one lookup per 4 KiB — a disk seek storm for a file that is physically
contiguous, which it almost always is.

### 4.2 Allocation policy

1. **Best-fit contiguous, with a rotating hint.** The allocator starts scanning
   from the last successful allocation, not from block 0. Without the hint,
   every allocation scans the whole bitmap.
2. **Extend the previous extent** when the newly allocated block is adjacent to
   it, which is what keeps files contiguous under normal use.
3. **Deferred allocation for writes.** With no delayed-allocation machinery,
   growing a file allocates immediately — the simple, correct choice for v0.9.

---

## 5. Directories

A directory is a file whose data blocks hold entries. Each entry is padded to an
8-byte boundary:

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | `inode` |
| 0x04 | 2 | `record_len` | Bytes to the next entry |
| 0x06 | 1 | `name_len` | 1..255 |
| 0x07 | 1 | `type` | Duplicated from the inode for `readdir` without a lookup |
| 0x08 | 1 | `checksum` | CRC32 of the name |
| 0x09 | 2 | `_pad` | |
| 0x0B | n | `name` | UTF-8, **not** NUL-terminated |

Entries are sorted by name within a directory block, so a lookup is a binary
search rather than a linear scan. Deleting an entry sets `inode = 0` and leaves
the record in place, so no compaction is needed and a partially written directory
block is still parseable.

**UTF-8 with a byte length, not a code-unit count.** A name is compared as bytes
and displayed as UTF-8, which means `ɛ` and `ɔ` work in file names without any
of the encoding machinery FAT32's UCS-2 long names require.

---

## 6. The journal

Every metadata change is written twice: once to the journal, once to its final
location. That is the entire cost, and it buys crash consistency.

```
   A metadata transaction:
   1. Write the new block contents to the journal as a transaction record.
   2. Write a commit record with a CRC32 and the sequence number.
   3. fsync.        ← until this completes, the old data is authoritative
   4. Write the blocks to their final locations.
   5. Mark the transaction complete in the journal (or let the next one overwrite it).
```

### 6.1 Replay

At mount, if the superblock does not say `CLEAN`:

* Scan the journal for records with a **valid CRC and a sequence number greater
  than the last replayed sequence**.
* Write each valid transaction's blocks to their final locations again. Replay is
  **idempotent**: writing the same block twice gives the same result, so a crash
  during replay is safe.
* A record with an invalid CRC was never fully written, so it is discarded — this
  is the commit point, and it is why the CRC exists.
* Mark the volume `CLEAN` and continue.

### 6.2 What is journalled

**Metadata only:** superblock, inodes, directory blocks, the bitmap, and the
indirect extent block.

File *data* is not journalled. This is the standard trade-off (the same one ext3
made with `data=ordered`): after a crash the file system structure is perfectly
consistent, and a file that was being written may contain partially updated
contents. Journalling 8 MiB of file data through a 4 MiB journal would require
blocking writes until the journal drains, which is a much worse user experience
for a much narrower guarantee.

---

## 7. Operations

```c
af_status_t afs_format(af_block_device_t *dev, af_u64 journal_blocks);
af_status_t afs_mount(af_block_device_t *dev, afs_mount_t **out);
af_status_t afs_unmount(afs_mount_t *mount);
af_status_t afs_sync(afs_mount_t *mount);

af_status_t afs_lookup(afs_mount_t *m, af_u32 dir_inode, const char *name,
                       af_u32 *out_inode);
af_status_t afs_create(afs_mount_t *m, af_u32 dir_inode, const char *name,
                       af_u16 type, af_u16 mode, af_u32 *out_inode);
af_status_t afs_unlink(afs_mount_t *m, af_u32 dir_inode, const char *name);
af_status_t afs_rename(afs_mount_t *m, af_u32 old_dir, const char *old_name,
                       af_u32 new_dir, const char *new_name);
af_status_t afs_read(afs_mount_t *m, af_u32 inode, af_u64 offset,
                     void *buf, af_size len, af_size *out_read);
af_status_t afs_write(afs_mount_t *m, af_u32 inode, af_u64 offset,
                      const void *buf, af_size len, af_size *out_written);
af_status_t afs_truncate(afs_mount_t *m, af_u32 inode, af_u64 size);
af_status_t afs_stat(afs_mount_t *m, af_u32 inode, afs_stat_t *out);
af_status_t afs_readdir(afs_mount_t *m, af_u32 dir_inode, af_u64 *cursor,
                        afs_dirent_t *out);
```

Permissions are checked by the file system service against the caller's uid and
gid, which the kernel supplies from the process. The syscall layer cannot be
bypassed to reach a block device: only the file system service holds the block
device capability (see
[../architecture/capability-model.md](../architecture/capability-model.md)).

---

## 8. Host-side tooling

| Tool | Purpose |
| --- | --- |
| `tools/mkafs.py` | Build an AFS image on the host, so the installed system can be pre-populated and a read-write root can be tested from the first boot |
| `tools/afriyie-fsck` | Verify the bitmaps and inode tables against each other, report and optionally repair. Used by the crash-consistency test and runnable on a suspect image |

Both are Python and are covered by the host test suite, which means the on-disk
format is exercised end to end without QEMU.

---

## 9. Testing

| Test | Tier | What it proves |
| --- | --- | --- |
| 500 randomised create/write/rename/delete operations, various sizes | T2 + host | Correctness under mixed churn |
| Unmount, remount, compare full directory tree and checksums | T2 | Data survives a clean cycle |
| Power-cut the VM mid-write, remount | T3 | The journal replays to a consistent state |
| Deliberately corrupt a journal transaction's CRC | T2 | The partial transaction is discarded, not applied |
| Kill the file system service mid-operation, let init restart it | T3 | Mount and replay recover correctly |
| Fill the volume | T2 | `AF_ERR_NOSPC`, no corruption |
| `mkafs.py` image mounted and read by the kernel | T2 | Host writer and kernel reader agree |
| `afriyie-fsck` on a kernel-written image | Host | The two implementations agree in both directions |

The last two matter most: they are cross-implementation checks, which catch
specification ambiguities that a single implementation cannot.

---

## 10. Deliberately deferred

| Feature | Why not at v0.9 |
| --- | --- |
| B-trees for directories | Binary search over sorted records is enough for thousands of entries and vastly simpler |
| Compression | No CPU budget to spare and no real need |
| Encryption | Needs a key-management story first; substantial work |
| Snapshots / copy-on-write | Powerful but a large design commitment; revisit after v1.2 |
| Extended attributes | Nothing needs them yet |
| fsck in the kernel | Recovery belongs on the host or in a user-space tool, not in Ring 0 |

---

## 11. References

* Rosenblum & Ousterhout, *The Design and Implementation of a Log-Structured File
  System* — journalling fundamentals
* ext3/ext4 documentation — the `data=ordered` metadata-only journalling rationale
* `docs/releases/v0.9.0.md` — acceptance evidence, once it exists
