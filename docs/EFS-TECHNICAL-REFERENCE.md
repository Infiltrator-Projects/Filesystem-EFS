# EFS: technical reference

Source evidence inspected at `429f5c572d5ca28096a29132c8886f24daf195c6`. Current implementation claims are source observations, not executed qualification.

## History and scope

SGI Extent File System (EFS) predates XFS and was used on IRIS/IRIX workstations and distribution media. It extends Unix inode semantics with runs of contiguous disk blocks rather than one pointer per block. Unlike XFS, EFS uses cylinder groups and a global free-block bitmap, not allocation-group free-space B+trees or the XFS log. Historical early EFS versions must be distinguished from the later 0x72959 and 0x7295a formats. The native project reader recognises the latter two. A project-private redo journal is a separate extension, not part of original SGI EFS.

## Logical geometry and media layout

A basic block is 512 bytes; EFS on-disk numbers are big-endian. Block zero is outside ordinary EFS allocation, the primary superblock is at block one, bitmap normally begins at block two, and cylinder groups begin at fs_firstcg. Each group contains a fixed number of inode blocks followed by allocatable data blocks. A replica superblock may occupy a reserved block near the volume end. SGI volume-header partitions and optical-image container metadata are outside EFS proper; provide the partition-relative slice. Do not confuse physical disk sectors or optical transfer blocks with EFS basic blocks.

## Superblock fields: geometry

Relative to byte 512, decode 32-bit filesystem size in basic blocks at 0; first cylinder-group block at 4; cylinder-group size at 8; 16-bit inode blocks per group at 12; sectors per track at 14; heads at 16; group count at 18; dirty state at 20; last update time at 24; magic at 28. Numeric values are big-endian. The first and last group must fit the declared filesystem and device; inode blocks must be nonzero and fewer than the group length. The historic IRIX 3.3 superblock extension introduced a second magic and moved/extended bitmap geometry fields. Interpret only supported revisions and verify the checksum before trusting offsets.

## Superblock fields: identity and free space

Six-byte filesystem and pack names occupy offsets 32 and 38. Bitmap byte length is at 44; free data-block count at 48; free-inode count at 52; bitmap block address at 56 (zero means default block two in the current reader); replica superblock address at 60; remaining original fields include allocation/search state and reserved bytes. The checksum is at offset 88. The current reader processes the preceding 88 bytes as big-endian 16-bit words, XORing each into a 32-bit accumulator and shifting left one bit with rotation for the later form; the older magic also accepts shift-only legacy checksum. Check the bitmap and replica do not overlap group allocations. These offsets describe the admitted source profile, not every historical EFS dialect.

## Inodes and location formula

Each inode is 128 bytes, four per basic block. Let I be inode blocks per cylinder group; there are 4I inode slots per group. For inode n, group = floor(n/(4I)); block = firstcg + group*cgsize + floor((n mod 4I)/4); byte within block = 128*(n mod 4). Root inode is 2. Within an inode: mode at 0, link count at 2, UID and GID at 4 and 6, 32-bit byte size at 8, access/modify/change times at 12/16/20, generation at 24, 16-bit extent count at 28, format/version and spare at 30/31, twelve eight-byte extent descriptors from 32. Special-device and inline symbolic-link interpretations depend on the inode type and format revision. Preserve native fields even when the host does not expose them.

## Extent records and indirect mapping

Each eight-byte descriptor stores an extent magic byte at 0, 24-bit physical basic-block address at 1, one-byte block count at 4 and 24-bit logical block offset at 5. The ordinary extent magic is zero. A nonempty run cannot cross the admitted cylinder-group data boundary or overlap its inode table. The one-byte length representation does not make every value legal for every SGI revision; native allocation constraints and the original manual govern writer limits. For at most twelve extents, descriptors directly map payload. For more than twelve, inode descriptors identify extent-table runs; the first descriptor's logical field gives the number of table descriptors. Each table block holds 64 extent records. Read only the declared number; validate table ownership separately from payload ownership.

Cross-check indirect extent counts, extent magic and the cylinder-group ownership boundaries. The original Linux EFS reader describes a four-byte directory block header and big-endian directory magic 0xbeef; directory slot offsets must not point into that header.

## File reads and mapping validation

Build an extent list in logical order and check it against the declared file size, actual block allocation and all reserved regions. A logical byte position p selects basic block floor(p/512) and byte offset p mod 512; find the run containing that logical block. Physical byte offset is 512*(physical_start + logical_block - logical_start) + remainder. For logical run [20,24) mapped at physical block 600, logical block 22 maps to block 602. For final partial blocks, truncate the read to EOF. Missing runs, duplicated physical ownership, illegal gaps or noncanonical extent order must not silently produce data. The current admitted reader validates descriptor and block counts and rejects unsupported layouts.

## Directories and namespace

Directories are extent-backed files organised into 512-byte blocks. Later directory blocks begin with 16-bit magic 0xBEEF; byte 2 records free-space position and byte 3 is slot count. The slot array begins at 4; a nonzero one-byte slot value gives half the byte offset of its entry. The entry stores a 32-bit inode number followed by one-byte name length and that many name bytes. Zero slot denotes unused. Reject entries crossing the block, overlapping the slot table or each other, invalid inode addresses and embedded NUL or path separator. Dot/dot-dot, unique names, directory parent relationships and link counts must be checked across the graph. Hard links are multiple references to one inode, not duplicate block ownership.

## Bitmap, allocation and repair

The native bitmap tracks free basic blocks; the current implementation interprets set bits as free and reconciles the count with superblock totals over each group's data range. Reserved boot, superblock, bitmap, inode and replica regions cannot be allocated even if damaged bits suggest otherwise. Reconstruct ownership from every live inode, including indirect tables and directory data; count hard-linked objects once and verify directory connectivity. Distinguish crosslinked blocks, leaked allocation, referenced-free blocks, stale counters and corrupt namespace. Original EFS recovery uses structural checking and superblock replica evidence; do not infer original transactional replay. A formatter must establish correct group geometry, inode tables, bitmap, root and replica before publishing a valid primary.

## Current Infiltrator provider scope

The inspected independent source contains CORE probe/mount/unmount/stat/list/read/write/create/remove/rename/truncate; VOLUME query, get/set label, sync, format, check; ALLOCATION map; FORMAT_PROFILE and DEFRAG map/plan/execute/verify/recover/release-plan. Only the hosted runtime profile is declared. Clean foreign EFS is read-only; writable mounts require the project journal and admitted geometry, with durability and exclusivity checked by transaction paths. Native format uses the 0x72959 variant, a six-character printable label limit and multi-extent layout. A Common V2 group descriptor is not release or firmware qualification. The current provider has one gated session; callers must respect operation masks and host-specific restrictions.

## Private journal: physical placement

The Infiltrator extension reserves a gap between the free bitmap and first cylinder group. With N basic blocks, the default bitmap has ceil(N/8) bytes; journal base is align_up(1024+ceil(N/8),4096). Its 64 pages of 4096 bytes occupy 256 KiB; first cylinder group begins after the journal. The first two pages alternate checksummed headers; up to 62 remaining pages hold redo images. Header magic EFJ2, version and sequence identify valid records. This scheme is not original SGI EFS. The provider refuses foreign writes and rejects inconsistent or unsupported journal geometry rather than treating padding as available storage.

## Private journal: durability and mutation

Build a prospective native bitmap without reusing old owned blocks before commit. Full 4096-byte redo images cover metadata and mixed physical pages; only exclusively previously-free payload pages may be written ahead of the journal. Persist redo images, flush, publish checksummed PREPARED header, then replay to validated home addresses and flush. A subsequent CLEAN header supersedes PREPARED. Alternate header slots preserve the prior durable state; validate checksums and destinations before replay. A transaction must include primary superblock page and update replica, bitmap and inode metadata consistently. Namespace operations must maintain directory parent links and link counts. A transaction exceeding the bounded redo capacity must fail or be split according to a separately proven safe operation contract.

## Defragmentation and growth reservations

The native plan simulates ownership from the actual bitmap without trusting external locations. It visits hard-linked files once, verifies the source extent layout and calculates a payload CRC before relocation. Execute moves one file at a time using copy-on-write allocation and redo-protected metadata; old extents remain owned until publication. Verify checks the intended physical layout and rereads payload bytes, then performs a structural consistency pass. A file may have multiple consecutive native descriptors despite a physically contiguous run because descriptor lengths are bounded. Distinguish physical fragmentation from descriptor count. Any growth reservation metadata is provider-specific and must remain protected from ordinary allocation; do not expose a private metadata file as a user file or claim reservations on foreign media.

## Implementation and qualification boundary

The independent provider uses Common V2: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate actual operation masks and media profiles. Distinguish unsupported, corrupt, I/O and stale-handle results. Require bounded host I/O, exclusive unmounted ownership and durable flush before mutation. Test Media and Defragmenter consume the same engine, not separate format parsers. Hosted and freestanding binaries, relocation, recovery, BOOT, kernel integration and packages require separate exact-source evidence. Current support belongs to the repository README and qualification records. The intended package paths are /System/Filesystems/ and /System/Resources/Documentation/; the reference is not an installation or release-status claim.

## References and independent test design

SGI original manual: https://www.typewritten.org/Manual/SGI/IRIX/6.5.3f/man4/efs.html ; inode layout https://techpubs.jurassic.nl/manpages_0530/cat4/inode.html ; architecture https://tech-pubs.net/SGI_Admin/books/IA_DiskFiles/sgi_html/apa.html ; reader cross-check https://github.com/torvalds/linux/tree/v6.12/fs/efs . Test original independently created IRIX images, both later magic values, direct and indirect extent transitions, cylinder boundary, bad checksum, invalid slot, overlapping directory entries, link aliases, bitmap disagreement, journal header corruption, replay interruptions, mixed-sector writes and no-space relocation. Compare payload and metadata to an independent reader; qualify actual packaged bytes and firmware separately. The remaining historical dialects, native maximum extent constraints and inline symlink cases require further primary-source validation.

The PDF preserves the earlier design and historical documents as explicitly historical appendices.

SGI's EFS preceded XFS in IRIX; XFS's transaction and allocation-group semantics are not interchangeable with EFS.

Retained source: https://github.com/Infiltrator-Projects/Filesystems/blob/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/efs/linux/efs.h (blob 918d2b9abb).
