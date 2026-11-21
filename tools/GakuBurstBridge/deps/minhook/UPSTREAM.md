# MinHook vendor source

Upstream: https://github.com/TsudaKageyu/minhook

Fixed revision recorded by the source project's Git gitlink:
`8af6b4acae5a9388fd742b56fa79ece89d96f823`.

This snapshot vendors an explicit source subset: LICENSE.txt, MinHook.h, the
buffer/hook/trampoline implementation and their headers, and HDE32/HDE64 source,
headers and tables. It includes no upstream Git metadata, build output or binary.
The observed local source bytes and their copyright headers are retained.
The license was completed separately from the official pinned upstream text. The Windows x64
build uses HDE64; HDE32 is retained as part of the same small source subset.

See [LICENSE.txt](LICENSE.txt) and the project [third-party notices](../../THIRD_PARTY_NOTICES.md).
Exact file hashes and independent upstream verification state are recorded in
[SOURCE_PROVENANCE.json](../../SOURCE_PROVENANCE.json); a declared gitlink alone
is not proof that a local working-tree file equals upstream.

## Evidence and limit

The upstream LICENSE.txt at this fixed revision was independently read through
the official raw GitHub URL and contains 73 lines: MinHook, HDE32 and HDE64.
This file is normalized UTF-8 without BOM and LF; its recorded SHA256 binds the
normalized copy, not the upstream transport bytes.

The implementation is an observed local vendor variant, not a pristine copy
of the declared base. Complete fixed-URL source text was read through the
browsing tool and C tokens (excluding comments/whitespace/BOM) were compared for
all 13 implementation/header/table files: 4 matched, 9 differed. See the exact
per-file method and differences in [UPSTREAM_COMPARISON.json](UPSTREAM_COMPARISON.json).
Differences include hook/thread code, trampoline details and HDE decoding/bounds
logic. This staging task preserves the existing local implementation; it does
not silently update a runtime dependency to the declared upstream revision.

Rendered-source comparison is not an authenticated Git-blob or raw transport
byte comparison. The Git tree API was unavailable and direct socket access was
blocked. The local exact SHA256 values freeze what this snapshot contains;
they do not repair the mismatch between the gitlink declaration and local code.
Resolve the exact fork/base and modification provenance before claiming a
pristine upstream dependency in a public source release.
