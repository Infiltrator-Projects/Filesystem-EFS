# Filesystem-EFS

Independent SGI Extent File System provider for Common V2, distinct from XFS. Hosted and import-free freestanding source supports original clean media reading and native journalled formatting, namespace mutation, allocation maps and defragmentation. Foreign media are not writable through the private journal profile. Growth policy 10 now creates native, physically adjacent allocation-owner files in a protected directory, validates their ownership and exposes their actual allocated blocks as RESERVED. Append and zero-extension consume the adjacent native reserve in place, transferring blocks between inodes in one transaction. Small overwrites preserve the reserve. Shrinks, oversized overwrites or growth beyond the reserved range use ordinary atomic copy-on-write and release the old reserve. The relocation and reserve-consumption interruption matrix checks both 512-byte and 4096-byte sectors, all observed write/flush boundaries, torn and uncertain writes, exact old/new physical maps and payloads, and refusal of a corrupt redo payload before home writes. Publication requires successful exact-revision CI, independent foreign-media evidence, portable package verification and native installer reconstruction. Qualification and release claims depend on exact executed evidence.

## Technical documentation

- [Engineering reference (PDF)](docs/EFS.pdf)
- [Technical reference (Markdown)](docs/EFS-TECHNICAL-REFERENCE.md)
- [PDF stream source](docs/EFS.pdfstreams) (plain text)
- [Native design and historical baseline](native/filesystems/efs/DESIGN.md)
- [Migration and current source status](MIGRATION_STATUS.md)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

Source operations, actual runtime masks and exact qualification evidence govern current support. The prior manuals are preserved in the PDF's historical appendices. Target installation: `/System/Filesystems/EFS.so` and `/System/Resources/Documentation/Filesystem-EFS.pdf`; the release workflow qualifies the exact portable and source-built installed bytes before publication.

The independent interoperability workflow uses a hash-pinned Rusty Backup CLI release solely as a test oracle: foreign formatted media are read through Common V2, and provider formatted media are independently checked and extracted. No oracle binary or invocation is part of production code or installation.
