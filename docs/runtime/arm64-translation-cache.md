# ARM64 translation and native block caching

The ARM64 backend on Windows x64 uses interpretation as its baseline. ARM64-to-x64 native blocks and disk reuse can be enabled explicitly. These features are independent of the image ART Java JIT. Experimental caching defaults to off; component correctness and application performance require separate validation.

## Configuration

| Environment variable | Behavior and default |
| --- | --- |
| `STONE_ARM64_BLOCK_JIT=1` | Enable in-process native blocks; off by default |
| `STONE_ARM64_DISK_CACHE=1` | Allow persistent reuse of hot static ELF blocks; off by default, requires native blocks |
| `STONE_ARM64_LOCAL_DIRECT=1` | Use a single-slot local index for comparison; the default is a four-way short-tag index |
| `STONE_ARM64_BLOCK_CHAIN=1` | Chain cache hits for static ELF blocks within the polling budget; off by default |
| `STONE_ARM64_STACK_STORE_TRACE=1` | Continue a dynamic-code block after stores only to checked guest stack memory; off by default |
| `STONE_ARM64_ELF_DATA_CACHE=1` | Cache data ownership for permanently published ELF images; off by default |
| `STONE_ARM64_COMPACT_PC=0` | Disable adjacent-PC address reuse for comparison; reuse is enabled by default |
| `STONE_ARM64_CACHE_EAGER=1` | Admit cached blocks early for comparison; the default uses the normal hotness threshold |
| `STONE_ARM64_CACHE_PRELOAD=1` | Read persistent artifacts during registration; the default loads lazily on actual block requests |
| `STONE_ARM64_CACHE_COMPRESSION=1` | Attempt payload compression before DPAPI protection; off by default |
| `STONE_ARM64_MAPPING_FIRST=0` | Query cached mapping hits after the current ELF/TLS for comparison; the default queries mappings first |
| `STONE_ARM64_PROFILE` | Sample interpreted and native block execution statistics; disable for performance baselines |
| `STONE_ARM64_CACHE_TIMING` | Time cache stages; disable for performance baselines |

These options affect the native execution backend, not ART Java AOT/JIT. See [diagnostics](../development/diagnostics.md) for diagnostic procedures.

## Native blocks and fallback

Native blocks carry the complete guest register, vector, PC, SP, and NZCV state. They cover general integer operations, branches, protected memory access, and SSE2 implementations of some integer NEON operations. Unsupported instructions, host calls, dynamic mapping changes, or faults return to the interpreter. Only unfinished instructions are executed; completed side effects are not repeated.

The normal block hotness threshold is 64 hits, and compilation is limited to 32 instructions per block. Execution remains subject to the 256-instruction signal polling boundary and bounded execution deadlines. Block chaining must not extend these boundaries.

Local lookup uses 64 buckets with four entries each and 32-bit short PC tags. Short tags only filter candidates. Every hit still checks the complete code owner, PC, load bias, original bytes, and mapping generation, and retains code ownership during execution. Eviction must not release code or unwind metadata still in use.

Adjacent-PC reuse only reduces constants and relocation entries. Independent constants are retained if signed imm32 cannot be used safely or wraparound occurs. Indirect returns, precise fault locations, and budget semantics remain intact. Actual instruction size and page-rounded allocation size are counted separately.

## Memory access, permissions, and exceptions

All generated memory accesses check ownership and permissions, distinguishing ELF, dynamic mappings, current-thread TLS, and guest stacks. Cached mapping ranges can be queried first. Cross-ELF data ownership caches recheck DataAt, TLS, RELRO, and code-page read permissions on every use; permissions and misses are not cached. Temporary unwinders use locked queries.

A dynamic-code stack-store proof is valid only if every write falls within the current non-executable guest stack and passes the original access checks. Writes to ordinary mappings, code, or writable code aliases end the block. A paired store invalidates the proof if either write fails these conditions. The proof cannot bypass fault, SP, or polling checks.

Generated code retains Windows unwind metadata. Executable pages and unwind tables are managed in batches, with reclamation outside the cache lock. Exceptions, implicit null checks, and suspend recovery preserve precise guest state. Exceptions must not cross generated code without unwind support.

## Persistent identity and authentication

Persistent blocks come only from static ELF files; dynamic ART JIT code is not written to disk. Source ELF, translator host, and CPU identities participate in validation. Loading rechecks source instruction ranges, bytes, relocations, and execution context. Filenames, short tags, or indexes cannot replace full identity validation.

Disk artifacts are authenticated with the current user's DPAPI. Saving reuses an authenticated file baseline; replacement by another process requires authentication and merging again. Files without stable identity are read in full. Corrupt, missing, or incompatible files fall back to in-process compilation without preventing native execution.

Publication uses atomic file replacement and flushes data. Existing read-only handles can continue reading the old file while new readers obtain the new one. If the filesystem cannot replace a file while retaining old handles, baseline handles are released and `MoveFileEx` is used; failure is reported as a save error. Saving is triggered by explicit execution-session completion, never by statistics output.

## Lazy loading and compression

By default, registration creates a bounded pending-load record. Artifacts are opened, authenticated, and parsed only on the first block query or recording request. Each bundle attempts its initial read at most once under the cache lock; aliases share a stable record. Eager admission requires the full hint index and therefore preloads it.

Optional XPRESS Huffman compression is inside DPAPI protection. Compressed payloads are saved only when smaller. Reading authenticates first, then checks magic, version, compressed length, and expanded length, with an expansion limit of `32 MiB + 64 KiB`. After decompression, the actual size is verified before checking block format, code identity, and relocations. Raw and compressed payloads can be read and merged together.

## Limits

Disk hits, fewer dispatches, and machine-code size changes do not directly establish application speedups. Experimental options default to off and must be evaluated for the specific host, image, and workload. Dynamic ART JIT code is never persisted, and full identity, permission, fault, and polling checks always remain in effect.
