# miniz retained sources

Upstream: https://github.com/richgel999/miniz
Version: 3.1.2
Tag commit: 77d0dce8627735138c51770d1799a1ef48f2117d
License: MIT; see LICENSE and the notices in each source file.

Upstream files are unmodified. Stone builds ZIP reading, inflate and checksums,
using MINIZ_NO_ZLIB_APIS, MINIZ_NO_DEFLATE_APIS and MINIZ_NO_STDIO. Disabling
deflate also disables archive writing. CMake generates the static miniz_export.h.
Unicode input uses a read callback; bounded output verifies ZIP CRC before
atomic publication. No build-time download or external runtime is required.

SHA-256:

`LICENSE` 0115478d567121238cf6cc1c0c361926cf07a49d9e4c9e66da97fac6a01646b3
`miniz_common.h` f1ba29821c8caef83585b328196aa346c5da7c82890a70a3d10d77c6cec0ed39
`miniz_tdef.h` d62da02953c3834dfcbde4db026c6d9345013ef9b0e53185131016a3a4e48c33
`miniz_tinfl.c` 2296ebd21ef9af5ebbefd5b454d4bb67d8fb4af2d24945e7fb32114374f75a76
`miniz_tinfl.h` da4850920fdf09f8877d9affabe1ebba1852ce4b1ebbd121f2ba7a7ea8e57e5d
`miniz_zip.c` b70e502c425e2a31146178c1466dccaf1874390009950774be2be9bd4ec908a9
`miniz_zip.h` b062a5def545445c73fd1d664e35a89165dfb0d996ab848f507244e819cfcf04
`miniz.c` f23204efee2bfaffb72a974d2e28c64c3c1d9787144b5c5890f129c465e467bd
`miniz.h` f0b67217bdd8e45b74eb9b7c2c7ae20ed6760a1d76f3276b93617039ebd082df
