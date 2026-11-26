# EFS Common V2 migration status

## Current source evidence

At `429f5c572d5ca28096a29132c8886f24daf195c6`, the native hosted implementation, journal, mutation and defrag source plus test suite and CI workflow exist. See [engineering reference](docs/EFS-TECHNICAL-REFERENCE.md) for capabilities and native/foreign distinctions. This documentation pass did not execute tests or verify a released package.

## Historical migration plan (preserved)


## Source and ownership

[Exact central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/efs). Editable Linux v6.12.107-derived source remains centrally; read-only compatibility is the initial target. Extract and adapt the existing implementation; preserve original licensing, attribution and provenance. New project-authored code uses GPL-3.0-or-later. No source transferred by this documentation commit.

## Qualification plan

1. Enumerate supported on-disk variants, validate geometry and features; implement bounded CORE probe.
2. Provide CORE mount/stat/list/read and VOLUME query using native superblock, inode layout, extent and indirect extent mapping, directories and symlinks.
3. Test independent EFS images, malformed extents and inode bounds; compare independent payloads and unchanged-image hashes; inject malformed media and I/O faults.
4. Implement mutation, format, check/repair and DEFRAG map/plan/execute/verify/recover only with proven allocation, durability and interruption/recovery semantics. Unsupported operations must refuse before writing.
5. Qualify exact packaged Common V2 ABI, hosted and import-free freestanding profiles, then actual BOOT and kernel integration independently.

Original historical status: all independent stages were pending; this is superseded by the source observations above.

The [design and qualification reference](docs/EFS.pdf) is available. The original design PDF is retained in the current manual's historical appendix.
