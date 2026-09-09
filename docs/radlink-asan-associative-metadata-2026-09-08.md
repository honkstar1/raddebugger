# ASan references to discarded associative metadata

## Reproduction and cause

BlankProgram's ASan-instrumented COFF contains a filename string in an associative COMDAT. Surviving metadata attached to other COMDAT owners references that string. When its owner loses duplicate selection, the filename section is discarded, leaving those references dangling.

The issue reproduces with the previously submitted 348ae713 linker, the 47a6150f prefix candidate, and the dev-merged 0763f173 candidate. It is not introduced by the prefix preference or the recent ICF fix. The failing response disables REF and ICF. Both compressed RLOBJ and reconstructed raw COFF inputs reproduce it.

LLVM lld-link 20.1.8 also rejects the discarded-section references. Microsoft link.exe accepts the same raw BlankProgram inputs. A small independent COFF fixture establishes the MSVC policy: a discarded associative target contributes RVA zero, section zero and section offset zero; its original symbol offset is discarded, but the relocation's addend and normal image-base adjustment remain. Thus an ADDR64 with addend 5 becomes image_base + 5, not a pointer to a different owner's metadata.

## Fix

- Keep the existing removed-section sentinel, adding an internal value tag to distinguish discarded associative symbols during address patching.
- Preserve that tag through symbol fixups.
- For non-debug relocations to tagged targets, use MSVC's zero-target semantics through the ordinary relocation calculation, retaining addends and base relocations.
- Keep ordinary removed-section errors and existing debug-relocation handling unchanged.
- Do not revive discarded sections, redirect metadata to another object, disable ASan, or ignore relocation errors globally.

This follows Microsoft's treatment of metadata the compiler has discarded; it does not recover the original missing filename string.

## Validation before packaging

- Native regression covers both input orders, REF enabled/disabled, a nonzero source-symbol offset, addends, ADDR64/ADDR32NB/SECREL/SECTION/REL32, live targets, and loader rebasing.
- Regression fails on 0763f173 and passes with the fix; the same final regression passes against Microsoft link.exe 14.51.36231.
- Linker/torture/base/eval2 suite: 183 passed, no failed/crashed tests, one existing exclusion (ms_link_icf_section_flag_eligibility, the previously documented Microsoft-linker comparison). Existing ordinary removed-section negative tests remain passing.
- Frozen BlankProgram ASan response links successfully with both compressed and raw objects, producing a 22,202,368-byte EXE and a 76,857,344-byte PDB in each case.

Reproduction files, compiler/linker comparison logs, SDK mappings and original-input hashes are kept locally under D:/devel/temp/radlink-asan-repro-20260908. Proprietary Engine objects are not part of this source change or any public fixture. The installed Engine linker, original response and original outputs are not replaced. No Engine runtime/farm/ASan-error-reporting soak result is claimed.
