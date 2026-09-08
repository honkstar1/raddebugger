# Combined radlink: complete dev merge, September 8, 2026

## Source lineage

This is a real merge of upstream `dev` at `47de6bf39376776a9914e42e5f75344a8841836a` into the combined release candidate `47a6150f486bfd724e4093062addd5f9167089e0`.

It preserves the previously submitted optimization/compressed-object stack (#842, #892, #932, #934, #935, #937), the COMDAT prefix fix (#939), and the complete upstream ancestry. It does not replace the combined source with stock dev.

## Integration decisions

- Adopt upstream's ICF relocation-target offset preservation, empty-input work guard, help updates, compiler/GCC portability changes, and workflow/build updates.
- Adopt the single-task dispatch fast path while preserving the combined shared-pool governor and barrier-drain fix from #937.
- Preserve the already-present BigObj, explicit DEFAULTLIB path, MSF serialized-extent and BLAKE3/LLVM assembly backports; resolve duplicate implementations without losing their existing tests and cleanup controls.
- Preserve the combined branch's script/native test organization. Port dev's new `icf_nonzero_local_reloc_targets_do_not_fold` regression into the native torture tests. Existing BigObj, MSF extent and deterministic-build coverage remains in place.
- Retain the existing full archive-symbol rescan and function-override subset rescan instead of upstream's persistent cursor variants where they conflict. This is deliberate correctness conflict resolution, not an omitted upstream ancestry change: symbols can change search type in place, and archive members can introduce references to older override directives. Remove unused cursor fields/initialization rather than keeping dead state.
- Retain the already-correct `void *`/`int` test comparator and migrated helper organization instead of resurrecting removed native-test helpers.

## Evidence for the rescan resolutions

Run the combined branch's existing script fixtures against the dev-based #939 linker (`8f88fa21`, which includes all of dev):

- `alt_name` fails: unresolved `override_archive_must_not_be_loaded`.
- `lib_search_rescans_changed_symbol` fails: expected `.target` output section is missing.

Both fixtures pass with this merged implementation. These are concrete regression checks for retaining the existing rescan protections while merging upstream.

## Validation

Clang/LLD 20.1.8, Release `-O2`, Oodle 2.9.16, VC toolset 14.51.36231 and Windows SDK 10.0.26100.0. No PGO or LTO; no performance gain is claimed.

- Combined linker/torture/base/eval2 selection: 182 passed, 0 failed/crashed, 1 excluded (`ms_link_icf_section_flag_eligibility`, the previously documented Microsoft-linker comparison).
- Upstream dev-based #939 native linker suite, directed at the merged binary: 141 passed, 0 failed/crashed, 1 excluded (`import_export`, whose upstream baseline assertion failure was separately reproduced earlier).
- Both suites include the ICF offset and COMDAT prefix regressions. Combined coverage includes compressed-object parity, archive rescans, shared-pool back-to-back passes and single-task dispatch.
- `bash -n build.sh` succeeds. No Linux/GCC runtime build is claimed by the Windows validation.

The packaged EXE/PDB are rebuilt after the merge commit to embed a clean source revision and are validated again. Exact hashes, final test receipts, build configuration and reproduction instructions belong to the release package. No Engine/farm installation, runtime soak, Wine/UBA validation or public binary upload is performed by this merge.
