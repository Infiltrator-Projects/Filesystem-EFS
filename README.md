# Filesystem-EFS

## 1. Purpose and format scope

Silicon Graphics Extent File System (EFS). This repository is the designated independent implementation home for InfiltratorOS. **Native Common V2 reader under development; no release or full qualification is claimed.**

## 2. On-disk architecture and invariants

Superblock geometry, inode records, direct and indirect extents, directory entries and symbolic links determine file mapping. Validate extent offsets, lengths and inode bounds before access. These are design requirements for the native implementation, not claims about code already present in this repository.

## 3. Common V2 implementation and capabilities

Native CORE probe, read-only mount/unmount, stat, enumeration and read; VOLUME query/label/native format, ALLOCATION map, FORMAT_PROFILE and DEFRAG extent mapping are implemented for development qualification. Native bitmap accounting and file-backed format regression are implemented; independent foreign-media, complete integrity verification and mutation/recovery qualification remain outstanding. The target entry point is `infiltrator_filesystem_api_v2`, using bounded, partition-relative host I/O. Implement native probe first, then mount, stat, enumeration, read and volume query where applicable. Consumers must negotiate supported groups and profiles; an unsupported operation must not report success or delegate to a private driver.

## 4. Integrity, mutation, allocation and recovery

Mutation, formatting, check/repair and DEFRAG map/plan/execute/verify/recover require format-specific allocation and ordering invariants, exclusive writable host access and demonstrated interruption recovery. Until qualified, reject them before writing. Preserve native disk semantics and do not add a kernel or FUSE fallback as an alternative filesystem authority.

## 5. Build, installation and release

The target installation is `/System/Filesystems/EFS.so` and `/System/Resources/Documentation/Filesystem-EFS.pdf`. Neither file is currently distributed by this repository. Once the implementation qualifies, the intended release is a portable amd64 DEB and CPU-native source-first RUN, with GitHub's automatic source archive. For development only, clone Libraries at 225e640ebed5351b1481bfbeef1a6144ca5146e7 into Libraries and run `make check`. Do not deploy this development provider.

## 6. Qualification and limitations

Independent EFS images, malformed extents and out-of-range inode references. Compare independent payloads and unchanged-image hashes; inject truncation, corruption and I/O faults. Establish exact-revision ABI, hosted and import-free freestanding qualification, then independently test BOOT and kernel integration. CI exercises the actual shared object against native structure fixtures and hostile metadata; foreign-media and packaging qualification remain outstanding.

## 7. Technical references and provenance

- [Design and qualification reference (PDF)](docs/EFS.pdf) — design-stage reference only, with [editable page source](docs/EFS.pages); not an operational manual.

- [Pinned central source baseline](https://github.com/Infiltrator-Projects/Filesystems/tree/101026021d9088d80b7f0f472d9061f0e2c688a3/native/filesystems/efs)
- [Native design](native/filesystems/efs/DESIGN.md)
- [Migration and qualification plan](MIGRATION_STATUS.md)
- [Original-format reference source](docs/EFS.pages) and [PDF](docs/EFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

Preserve inherited notices and licences on source extraction; new project-authored code uses GPL-3.0-or-later. The original-format reference is available here; installation of the PDF remains pending. Central code or test results are not evidence of an independent provider.
