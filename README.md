# Filesystem-EFS

Independent SGI Extent File System provider for Common V2, distinct from XFS. Hosted source supports original clean media reading and native journalled formatting, namespace mutation, allocation maps and defragmentation. Foreign media are not writable through the private journal profile. Qualification and release claims depend on exact executed evidence, not this documentation pass.

## Technical documentation

- [Engineering reference (PDF)](docs/EFS.pdf)
- [Technical reference (Markdown)](docs/EFS-TECHNICAL-REFERENCE.md)
- [PDF stream source](docs/EFS.pdfstreams) (plain text)
- [Native design and historical baseline](native/filesystems/efs/DESIGN.md)
- [Migration and current source status](MIGRATION_STATUS.md)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

Source operations, actual runtime masks and exact qualification evidence govern current support. The prior manuals are preserved in the PDF's historical appendices. Target installation: `/System/Filesystems/EFS.so` and `/System/Resources/Documentation/Filesystem-EFS.pdf`; packaging is not established by this documentation update.
