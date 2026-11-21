# Source selection and review

Source checkpoint: `39427552c030c631a0ddf10edbc12d705b057e3d`.

The input repository was clean. Project files were selected individually from
that immutable Git tree, not copied recursively from a working directory.
Runtime source first entered the standalone source history at
`5f3de8b6ff0404c75d8958b88384ec020c32f8db`; older local history records bridge
extraction at `5985ea50a5af64b1a1d2e78e72a63b3ed41a836b` and Quartz/Sleeve support
at `91373c596b4ee8cde58a47206ceecba1339bf427`. Those objects were read as history;
their repository metadata is not included here.

## Reviewed scope

- Runtime C++ and C ABI: project implementation, no asset payload. Fixed field
  offsets, HIDs, RVA/opcode patterns and module names are bounded compatibility
  facts. Runtime hooks act only when a caller configures/publishes valid input.
- Native tests: synthetic arrays, callbacks, controlled PE memory and absence-of-
  GameAssembly checks. The ABI executable loads only the DLL passed by build.ps1.
- Python tools: standard-library static PE/metadata inspection; no network,
  process connection or game DLL loading. Tools write only explicit output paths.
- Python tests: construct PE bytes and JSON in TemporaryDirectory; no captured
  user input. All test temporary output must be directed into a fresh build path.
- Vendor: MinHook/HDE source and original license/copyright headers; separate
  third-party classification and fixed-upstream verification recorded per file.
- Documentation: independent build and current integration contracts. Old
  MAINTENANCE_SOP research procedures, machine paths, past measured visual values
  and ONBOARDING_VERIFICATION history were not copied. The maintainer page is a
  new portable contract. The integration document omits its historical live-data
  paragraph; all API/thread/fallback semantics are preserved.

## Staging modifications

No runtime, C ABI, profile, native test or Python implementation behavior was
changed. build.ps1 only replaces the missing-submodule error with guidance to
restore this vendored source snapshot. README/NOTICE/governance are self-contained.
The new Git repository has no remote and no borrowed Git object directory.

## Manifest interpretation

SOURCE_PROVENANCE.json binds selected file bytes to review notes and source Git
blob identities. Staging-authored files have their own classification. The
manifest excludes itself to avoid a self-hash; the Git checkpoint binds its bytes.
Local validation outputs/logs are excluded from the source allowlist. This is a
source inclusion review, not a signature or a grant for unrelated third-party
assets. Any public distribution remains a separate owner action.

## Vendor evidence addendum

The source checkpoint vendor LICENSE.txt contained only the MinHook section.
The staging license restores the full 73-line MinHook/HDE32/HDE64 text from the
official fixed-revision raw GitHub source, normalized to UTF-8/LF. No vendor C
implementation was changed. The fixed revision remains a declaration from
the source gitlink. Full rendered upstream C-token comparison then found that
9 of 13 implementation files differ from that declared revision. This local
vendor variant is retained byte-for-byte and the differences are recorded in
deps/minhook/UPSTREAM_COMPARISON.json. Exact fork/base and modification provenance
remain unresolved for any future pristine-upstream claim or public source review.
No dependency code was silently updated as part of staging.

## Product documentation maintenance

The initial source snapshot at 11a771b preserved code but made the README mainly
about staging/audit and shortened the mature maintainer guide to 26 lines.
Documentation is now restored for ordinary users and developers. The recovery
source is GakuBurstBridge 39427552c030c631a0ddf10edbc12d705b057e3d README.md and
docs/MAINTENANCE_SOP.md, adapted for the independently vendored source layout.

The product README leads with use/install/build. New docs cover installation,
upgrade/removal, troubleshooting, component compatibility and first development.
The portable maintainer guide with 18 sections restores the earlier technical workflow,
input schemas, diagnosis and rollback/performance explanations; it omits machine
paths, the old live UDP sender and a few captured numerical history paragraphs.
There is no matching downloadable product bundle yet; local test DLLs are not
represented as releases. This is a delivery gap, not another source-audit gate.

No compiled input, C ABI, vendor, profile or test implementation changed from 11a771b.
The independent local-clone build results remain valid for those exact code
bytes; whole suites were not rerun solely for prose changes. Four readonly CLI
flows were executed on newly authored synthetic PE/JSON to check the restored
commands. Current source selection hashes include these document changes.
Documentation records are marked author-reviewed maintenance, not invented
independent approval or new game acceptance. The 11a771b commit is retained.
