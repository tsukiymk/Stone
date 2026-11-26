# Android error descriptions

[`errno-descriptions.inc`](../../oss/bionic/errno-descriptions.inc) adapts AOSP Bionic's error description data for the
Windows bridge. It retains the upstream BSD 2-Clause license and copyright.
No Bionic process bootstrap, thread layout, or libc executable code is copied.

Upstream: <https://github.com/aosp-mirror/platform_bionic>, `main`. Exact Git blob identities used:

| File | SHA-1 |
| --- | --- |
| `libc/private/bionic_errdefs.h` | `435d49bc3091b9f83caaec5040cd4256be33aafc` |
| `libc/kernel/uapi/asm-generic/errno-base.h` | `592edb0dcfc2d1ca0980b753096615da31aa169c` |
| `libc/kernel/uapi/asm-generic/errno.h` | `f1e18ad62136be696b1e7121e1835f9f94648ac5` |

Adaptation: replace each `__BIONIC_ERRDEF(name, description)` entry with
`STONE_ERRNO_DESCRIPTION(number, description)`, using the numeric definition
from the two asm-generic headers. Keep the original name as a comment. Preserve
all 132 entries, including zero; leave the unassigned values 41 and 58 absent.
The numeric values apply to Android x86_64 and ARM64. Windows errno macros must
never be used to index this data.

The buffer and return-value behavior follows Bionic's
[`strerror.cpp`](https://android.googlesource.com/platform/bionic/+/refs/heads/main/libc/bionic/strerror.cpp),
Git blob `c41b54784df0c4ba16160ac17b0014db66175125`.
Stone's own implementation keeps a Windows thread-local buffer for unknown
error numbers because Android's Bionic pthread/TLS layout is not initialized.
