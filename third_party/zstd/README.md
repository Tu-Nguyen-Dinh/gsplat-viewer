# zstd decoder (vendored)

Decompression part of [Zstandard](https://github.com/facebook/zstd) **v1.5.7**, taken unmodified
from the official release `zstd-1.5.7.tar.gz`
(SHA-256 `eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3`):
`lib/zstd.h`, `lib/zstd_errors.h`, `lib/common/*` (without the thread pool) and `lib/decompress/*`
(without the x86-64 assembly fast path; built with `ZSTD_DISABLE_ASM`).

Used only to read `.spz` version 4 files, whose attribute streams are zstd-compressed.
License: BSD (see LICENSE); zstd is dual-licensed BSD / GPLv2, this project uses it under BSD.
