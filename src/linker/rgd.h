// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

// RAD Group Digest (.rgd) -- on-disk staging artifact.
//
// A digest is a compact, sequential, ready-to-consume binary that carries a group of objs'
// already-parsed / already-hashed / pre-sorted contents with ALL global decisions left UNRESOLVED
// (symbol winner selection, COMDAT/ICF folding, type-index numbering, image layout, deferred-reloc
// apply). This is the ThinLTO "summary index" model applied to native COFF linking: the stage step
// runs radlink's existing per-obj front-end once per group and serializes the result; the final link
// runs the irreducible global work over ~40 digests instead of re-parsing ~2000+ raw objs.
//
// Design invariants (see radlink_group_digest_format.md):
//  - Little-endian, fixed-layout, naturally-aligned structs -> consumed by direct cast from the mmap.
//    No pointers; all cross-references are indices / byte offsets.
//  - Every sub-stream is page-aligned and self-contained.
//  - READ-ONLY: the link never writes into a .rgd -> map PAGE_READONLY / FILE_MAP_READ (shared) ->
//    zero private-dirty pages, zero CoW faults, near-free teardown. This is the property that removes
//    the measured Mi* fault storm + the exit tail for digest inputs (raw objs are CoW; digests are not).
//  - Pure function of (source obj bytes, link config) -> content-addressable, reproducible, and
//    bit-identical to a clean link.
//
// Phase 1 (this header): symbol + section + reloc digest (the parse/paging win, type-independent).
// Types (RRT) and per-module PDB streams are wired in later phases; their stream slots are reserved.

#define RGD_MAGIC   "RAD-GRP-DIGEST\0"  // 16 bytes incl. terminator
#define RGD_VERSION 1

#define RGD_PAGE_SIZE 4096
#define RGD_BAD_IDX   max_U32

typedef enum RGD_Stream
{
  RGD_Stream_Names,
  RGD_Stream_BoundarySyms,
  RGD_Stream_Comdats,
  RGD_Stream_Sections,
  RGD_Stream_Contribs,
  RGD_Stream_SectionData,
  RGD_Stream_Types,          // reserved (phase 2: embed LNK_RRT)
  RGD_Stream_DeferredRelocs,
  RGD_Stream_Directives,
  RGD_Stream_DebugModules,   // reserved (phase 4)
  RGD_Stream_Provenance,
  RGD_Stream_COUNT
} RGD_Stream;

typedef struct RGD_StreamDir
{
  U64 off;  // byte offset from file start (page-aligned)
  U64 size; // byte size of the sub-stream
} RGD_StreamDir;

typedef struct RGD_Header
{
  U8   magic[16];            // RGD_MAGIC
  U32  version;              // RGD_VERSION
  U32  machine;              // COFF_MachineType
  U128 content_hash;         // blake3(all source obj bytes + config_hash) -- the cache key
  U32  config_hash;          // hash of link-relevant config knobs
  U32  group_id;             // stable partition id (by module/dir)
  U64  source_obj_count;
  U32  section_count;        // # RGD_Section entries
  U32  boundary_sym_count;   // # RGD_Sym entries
  U32  comdat_count;         // # RGD_Comdat entries
  U32  type_index_base;      // group-local TI numbering start (== CV_MinComplexTypeIndex)
  RGD_StreamDir streams[RGD_Stream_COUNT];
} RGD_Header;

// --- Names -------------------------------------------------------------------
// Blob of length-prefixed UTF-8 strings (U32 len; U8 bytes[len]), deduplicated. Referenced by
// U32 name_off (byte offset into the Names stream). Hashes are carried on the records that need them
// (symbols) so resolution never re-hashes.

// --- BoundarySyms -- sorted ascending by trie_hash ---------------------------
// ONLY the symbols the global link must see (External / Weak / Common / Abs / Undefined / COMDAT).
// Purely-internal symbols (Static Regular, LnkRemove-section, defined+used only in-group) are bound at
// stage and omitted -- the global trie never touches them.
typedef struct RGD_Sym
{
  U64 trie_hash;       // precomputed lnk_symbol_table_hasher(name)
  U32 name_off;        // -> Names
  U32 flags;
  U64 value;
  U32 section_number;  // group-local; 0 for undef/common/abs per interp
  U16 type;            // COFF_SymbolType
  U8  storage_class;   // COFF_SymStorageClass
  U8  interp;          // COFF_SymbolValueInterpType
  U32 comdat_idx;      // -> Comdats, or RGD_BAD_IDX
  U32 def_obj_idx;     // group-local origin obj. For section-bound (Regular) syms it pairs with
                       // section_number to locate the defining contrib at consume; also remaps weak_tag.
  U32 weak_char;       // COFF_WeakExtType for Weak interp; else 0
  U32 weak_tag;        // Weak interp: -> BoundarySyms (the default/tag symbol, post-sort); else RGD_BAD_IDX
} RGD_Sym;

// --- Comdats -----------------------------------------------------------------
// All COMDAT candidates emitted unresolved; final link runs selection (tie-break needs the group's
// global input_idx base -- see Provenance). For ExactMatch, the bytes live in SectionData.
typedef struct RGD_Comdat
{
  U16 select;          // COFF_ComdatSelectType
  U16 _pad;
  U32 length;
  U32 checksum;
  U32 section_idx;        // -> Contribs (the contrib this comdat owns)
  U32 leader_sym_idx;     // -> BoundarySyms, or RGD_BAD_IDX
  U32 associate_sect_idx; // COFF_ComdatSelect_Associative: obj-local sect idx this associates with; else RGD_BAD_IDX
} RGD_Comdat;

// --- Sections -- output-section directory, sorted by (name, sort_idx) --------
typedef struct RGD_Section
{
  U32 name_off;        // ".text", ".rdata", ... (lexical $-group folded into sort_idx)
  U32 sort_idx;        // PE $-group lexical order
  U32 flags;           // COFF_SectionFlags
  U32 align;
  U64 total_size;      // group's size for this section, pre-aligned to file_align
  U32 contrib_first;   // -> Contribs
  U32 contrib_count;
  U64 data_off;        // -> SectionData (this section's concatenated bytes)
} RGD_Section;

// --- Contribs -- one per source obj section (reconstruction model) -----------
// Emitted in (source_obj_idx, obj_sect_idx) order. The consume side feeds these through radlink's
// normal gather/sort/layout pipeline (sort key = Compose64Bit(obj_idx, obj_sect_idx)) so the final
// output is byte-identical by construction. (A later phase pre-groups/pre-lays-out for the layout win.)
typedef struct RGD_Contrib
{
  U32 source_obj_idx;  // group-local obj index (determinism key high)
  U32 obj_sect_idx;    // section index within that obj (determinism key low)
  U32 name_off;        // -> Names (full section name incl. $-group, e.g. ".text$mn")
  U32 flags;           // COFF_SectionFlags (content/align/link bits)
  U32 align;           // bytes
  U32 reloc_first;     // -> DeferredRelocs (this contrib's relocs), or 0 with reloc_count==0
  U32 reloc_count;
  U32 _pad;
  U64 vsize;           // virtual size
  U64 data_off;        // -> SectionData; valid iff data_size>0 (bss/uninit has data_size==0)
  U64 data_size;       // = fsize (file bytes)
} RGD_Contrib;

// --- SectionData -------------------------------------------------------------
// Raw section bytes, concatenated per output section in contrib order, pre-aligned to file_align, with
// intra-group Rel32/Rel32_1..5 relocations ALREADY APPLIED (the symbol_voff - reloc_voff delta is
// invariant under group relocation). Final image fill = direct MemoryCopy of each group's blob.

// --- DeferredRelocs ----------------------------------------------------------
// Every reloc, classified at stage. The apply site is addressed by (apply_obj_idx, apply_obj_sect_idx,
// apply_off) -> consume maps (obj,sect)->contrib->final image offset. The addend is read from
// SectionData at apply time (x64 relocs read the existing bytes), so it is NOT stored here.
// Target is one of:
//   _Boundary: target_boundary_sym -> (sorted) BoundarySyms; resolved by the global trie at final.
//   _Internal: same-obj static target {target_sect_idx, target_value} (isymbol is obj-local), resolved
//              group-locally via the target section's contrib voff + value.
typedef enum RGD_RelocTarget
{
  RGD_RelocTarget_Boundary,
  RGD_RelocTarget_Internal,
} RGD_RelocTarget;

typedef struct RGD_Reloc
{
  U32 apply_obj_idx;        // contrib owner (== target obj for _Internal; isymbol is obj-local)
  U32 apply_obj_sect_idx;
  U64 apply_off;            // offset within the apply contrib's data
  U16 type;                 // COFF_RelocType (machine-specific)
  U16 target_kind;          // RGD_RelocTarget
  U32 target_boundary_sym;  // _Boundary: -> BoundarySyms (post-sort); _Internal: RGD_BAD_IDX
  U32 target_sect_idx;      // _Internal: target obj_sect_idx; else 0
  U32 _pad;
  S64 target_value;         // _Internal: target symbol value (offset into target section); else 0
} RGD_Reloc;

// --- Directives --------------------------------------------------------------
// Parsed .drectve tokens (defaultlib / include / alternatename / merge / export / ...), to REPLAY into
// config at merge time (obj load currently mutates global config; that side effect is re-applied, not
// baked into symbol state). Stored as a Names-style length-prefixed string blob, one token per entry.

// --- Provenance -- cache invalidation (ThinLTO 4-condition) ------------------
typedef struct RGD_Provenance
{
  U64 config_hash;
  U64 obj_hash_count;  // followed by U128 source_obj_hashes[obj_hash_count]
  U64 export_count;    // followed by U64 export_name_hashes[export_count]  (sorted)
  U64 import_count;    // followed by U64 import_name_hashes[import_count]  (sorted, = undefineds)
} RGD_Provenance;

////////////////////////////////
//~ Builder / Reader

typedef struct RGD_SymNode  RGD_SymNode;
struct RGD_SymNode { RGD_SymNode *next; RGD_Sym sym; String8 name; };

typedef struct RGD_ContribNode  RGD_ContribNode;
struct RGD_ContribNode { RGD_ContribNode *next; RGD_Contrib contrib; };

typedef struct RGD_RelocNode  RGD_RelocNode;
struct RGD_RelocNode { RGD_RelocNode *next; RGD_Reloc reloc; }; // target_boundary_sym holds PUSH-order
                                                                // sym idx until rgd_serialize remaps it

typedef struct RGD_ComdatNode  RGD_ComdatNode;
struct RGD_ComdatNode { RGD_ComdatNode *next; RGD_Comdat comdat; };

// In-memory accumulator the stage step fills before serialization.
typedef struct RGD_Builder
{
  Arena         *arena;
  U32            machine;
  U32            group_id;
  U64            source_obj_count;

  // interned names
  HashTable     *name_ht;     // String8 -> U32 name_off
  String8List    names;
  U64            names_size;

  // accumulated symbols
  RGD_SymNode   *first_sym;
  RGD_SymNode   *last_sym;
  U64            sym_count;

  // accumulated section contributions
  RGD_ContribNode *first_contrib;
  RGD_ContribNode *last_contrib;
  U64              contrib_count;

  // concatenated section bytes (-> SectionData)
  String8List    section_data;
  U64            section_data_size;

  // accumulated relocs
  RGD_RelocNode *first_reloc;
  RGD_RelocNode *last_reloc;
  U64            reloc_count;

  // accumulated comdats
  RGD_ComdatNode *first_comdat;
  RGD_ComdatNode *last_comdat;
  U64             comdat_count;

  // .drectve tokens to replay (-> Directives), each length-prefixed (Names-style)
  String8List    directives;
} RGD_Builder;

// Parsed view over a mapped .rgd. All pointers alias the read-only mapping.
typedef struct RGD_Parsed
{
  String8        data;        // the whole mapped file (read-only)
  RGD_Header    *header;
  String8        names;       // RGD_Stream_Names bytes
  RGD_Sym       *syms;        // RGD_Stream_BoundarySyms (header->boundary_sym_count)
  RGD_Comdat    *comdats;     // header->comdat_count
  RGD_Section   *sections;    // header->section_count
  RGD_Contrib   *contribs;
  U64            contrib_count;
  String8        section_data;
  RGD_Reloc     *relocs;
  U64            reloc_count;
  String8        directives;
} RGD_Parsed;

internal U32        rgd_name_off(RGD_Builder *b, String8 name);   // intern -> Names offset
internal String8    rgd_name_from_off(RGD_Parsed *p, U32 off);

// push a section contribution; section_bytes is the raw file data (empty for bss). Returns the
// SectionData offset (or 0 when empty). Caller fills contrib.reloc_first/reloc_count separately.
internal U64         rgd_builder_push_contrib(RGD_Builder *b, RGD_Contrib contrib, String8 section_bytes);
internal void        rgd_builder_push_reloc(RGD_Builder *b, RGD_Reloc reloc); // for _Boundary set target_boundary_sym = push-order sym idx
internal void        rgd_builder_push_comdat(RGD_Builder *b, RGD_Comdat comdat);
internal void        rgd_builder_push_directive(RGD_Builder *b, String8 token);

internal String8List rgd_serialize(Arena *arena, RGD_Builder *b);  // -> file bytes (page-aligned streams)
internal RGD_Parsed  rgd_parse(String8 data);                      // validate magic/version, slice streams
internal B32         rgd_is_valid(RGD_Parsed *p);
