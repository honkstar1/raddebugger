// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

// --- Input -------------------------------------------------------------------

typedef struct LNK_SymbolNameCache
{
  U64 *masks;
  U32 *block_bases;
  U32 *name_sizes;
} LNK_SymbolNameCache;

// /OPT:ICF fold record (one per section, indexed by section_number-1), filled at fold-apply.
// Distinguishes ICF folds from same-name COMDAT selection and /OPT:REF removal (all three end
// up LnkRemove'd with a redirected symlink, but only ICF folds join DIFFERENT-named sections,
// which is what the debug-info aliasing below needs to know). set==0 means not ICF-folded.
typedef struct LNK_ICFFold
{
  U32 leader_obj_idx; // input_idx of the leader's obj
  U32 leader_sn;      // leader section number
  B8  set;
} LNK_ICFFold;

typedef struct LNK_Obj
{
  String8 path;
  String8 data;

  COFF_FileHeaderInfo header;
  COFF_SectionFlags  *section_flags;
  LNK_SymbolNameCache symbol_name_cache;

  // flags
  B8 hotpatch;
  B8 exclude_from_debug_info;

  U32 input_idx;

  // COMDAT
  U32                 *comdats;
  U32Node            **associated_sections;
  LNK_ObjSymbolRef   *symlinks;
  LNK_ICFFold        *icf_fold;       // /OPT:ICF fold map (per section, sn-1 indexed); 0 if ICF off
  B8                 *icf_lines_only; // .debug$S sections associated to an ICF-folded function: stay
                                      // LnkRemove'd, but merge into the module remapped to the leader RVA
                                      // (sect_idx indexed; 0 array ptr when ICF off / no folds).
                                      // 1 = C13 Lines only (source breakpoints bind); 2 = full record
                                      // tree (fold joins a DIFFERENT source location and has locals --
                                      // watch-window labels come from the right source)

  // link
  struct LNK_LibMemberRef *link_member;
  struct LNK_ObjNode      *self;

  // Private (reloc-patched) copies of .debug$* section data, indexed by sect_idx; null when the
  // obj has no reloc-patched debug sections. Populated in lnk_obj_reloc_patcher so debug relocs
  // never dirty the copy-on-write input mapping; debug-section readers fetch bytes through
  // lnk_obj_get_sect_data which prefers the copy when present.
  String8 *sect_data_copies;

  // type info
  U32 debug_t_sect_idx;
  U32 debug_p_sect_idx;
  U32 debug_h_sect_idx;

  // ICF
  U32 llvm_addrsig_sect_idx;

  // @type_server
  Rng1U64         ti_range;
  CV_TypeIndex   *ti_map;
  Rng1U64         pch_ti_range;
  U64             pch_obj_idx;
} LNK_Obj;

typedef struct LNK_ObjSection
{
  LNK_Obj            *obj;
  U64                 sect_idx;
  U64                 section_number;
  COFF_SectionHeader *header;
  COFF_SectionFlags  *flags;
  Rng1U64             vrange;
  Rng1U64             frange;
  U32                 reloc_count;
} LNK_ObjSection;

typedef struct LNK_ObjNode
{
  struct LNK_ObjNode *next;
  struct LNK_ObjNode *prev;
  LNK_Obj             data;
} LNK_ObjNode;

typedef struct LNK_ObjList
{
  U64          count;
  LNK_ObjNode *first;
  LNK_ObjNode *last;
} LNK_ObjList;

typedef struct LNK_ObjNodeArray
{
  U64          count;
  LNK_ObjNode *v;
} LNK_ObjNodeArray;

// --- Directive Parser --------------------------------------------------------

typedef struct LNK_Directive
{
  struct LNK_Directive *next;
  String8               id;
  String8List           value_list;
} LNK_Directive;

typedef struct LNK_DirectiveList
{
  U64            count;
  LNK_Directive *first;
  LNK_Directive *last;
} LNK_DirectiveList;

typedef struct LNK_DirectiveInfo
{
  LNK_DirectiveList v[LNK_CmdSwitch_Count];
} LNK_DirectiveInfo;

// --- Workers Contexts --------------------------------------------------------

typedef struct
{
  struct LNK_Input **inputs;
  LNK_ObjNode       *objs;
  U64                obj_id_base;
  U32                machine;
  B32                find_debug_t;
  B32                find_llvm_addrsig;
} LNK_ObjIniter;

typedef struct
{
  LNK_SymbolTable  *symtab;
  LNK_Obj         **objs;
} LNK_InputCoffSymbolTable;

typedef struct
{
  LNK_Obj    **objs;
  String8      name;
  B32          collect_discarded;
  String8List *out_lists;
} LNK_SectionCollector;

// --- Error -------------------------------------------------------------------

internal String8 lnk_loc_from_obj(Arena *arena, LNK_Obj *obj);
internal void lnk_error_obj(LNK_ErrorCode code, LNK_Obj *obj, char *fmt, ...);
internal void lnk_error_input_obj(LNK_ErrorCode code, struct LNK_Input *input, char *fmt, ...);

// --- Input -------------------------------------------------------------------

internal LNK_Obj ** lnk_array_from_obj_list(Arena *arena, LNK_ObjList list);
internal void       lnk_obj_list_push_node_many(LNK_ObjList *list, U64 count, LNK_ObjNode *nodes);
internal void       lnk_obj_list_push_node(LNK_ObjList *list, LNK_ObjNode *node);

internal void       lnk_inputer_push_obj_symbols(TP_Context *tp, TP_Arena *arena, LNK_SymbolTable *symtab, U64 objs_count, LNK_ObjNode *objs);

// --- Metadata ----------------------------------------------------------------

internal U32              lnk_obj_get_features(LNK_Obj *obj);
internal U32              lnk_obj_get_comp_id(LNK_Obj *obj);
internal U32              lnk_obj_get_vol_md(LNK_Obj *obj);
internal struct LNK_Lib * lnk_obj_get_lib(LNK_Obj *obj);
internal String8          lnk_obj_get_lib_path(LNK_Obj *obj);
internal U32              lnk_obj_get_removed_section_number(LNK_Obj *obj);
internal B32              lnk_obj_get_comdat_symlink(LNK_Obj *obj, U64 section_number, LNK_ObjSymbolRef *symlink_out);
internal U32List          lnk_obj_collect_associated_sections(Arena *arena, LNK_Obj *obj, U32 root_section, COFF_SectionFlags skip_flags);

// --- Symbol & Section Helpers ------------------------------------------------

internal COFF_SectionHeader * lnk_coff_section_header_from_section_number(LNK_Obj *obj, U64 section_number);
internal force_inline COFF_ParsedSymbol lnk_parsed_symbol_from_coff_symbol_idx(LNK_Obj *obj, U64 symbol_idx);
internal force_inline COFF_ParsedSymbol lnk_parsed_symbol_from_coff_symbol_idx_no_name(LNK_Obj *obj, U64 symbol_idx);
internal force_inline String8           lnk_symbol_name_from_coff_symbol_idx(LNK_Obj *obj, U64 symbol_idx);
internal U64                  lnk_obj_sect_idx_from_section_number(LNK_Obj *obj, U64 section_number);
internal U64                  lnk_obj_section_number_from_sect_idx(LNK_Obj *obj, U64 sect_idx);
internal String8              lnk_obj_section_name_from_section_number(LNK_Obj *obj, U64 section_number);
internal String8              lnk_obj_section_name_from_sect_idx(LNK_Obj *obj, U64 sect_idx);
internal LNK_ObjSection       lnk_obj_section_from_sect_idx(LNK_Obj *obj, U64 sect_idx);
internal String8              lnk_obj_get_sect_data(LNK_Obj *obj, U64 sect_idx, Rng1U64 frange);
internal LNK_ObjSection       lnk_obj_section_from_section_number(LNK_Obj *obj, U64 section_number);
internal COFF_RelocArray      lnk_coff_relocs_from_section_header(LNK_Obj *obj, COFF_SectionHeader *section_header);
internal String8              lnk_coff_string_table_from_obj(LNK_Obj *obj);
internal String8              lnk_coff_symbol_table_from_obj(LNK_Obj *obj);
internal B32                  lnk_try_comdat_props_from_section_number(LNK_Obj *obj, U32 section_number, COFF_ComdatSelectType *select_out, U32 *section_number_out, U32 *section_length_out, U32 *check_sum_out);

// --- Helpers ----------------------------------------------------------------- 

internal String8List * lnk_collect_obj_sections(TP_Context *tp, TP_Arena *arena, U64 objs_count, LNK_Obj **objs, String8 name, B32 collect_discarded);
internal B32           lnk_obj_is_before(void *raw_a, void *raw_b);

// --- Directive Parser --------------------------------------------------------

internal void              lnk_parse_msvc_linker_directive(Arena *arena, LNK_Obj *obj, LNK_DirectiveInfo *directive_info, String8 buffer);
internal String8List       lnk_raw_directives_from_obj(Arena *arena, LNK_Obj *obj);
internal LNK_DirectiveInfo lnk_directive_info_from_raw_directives(Arena *arena, LNK_Obj *obj, String8List raw_directives);

// --- Debug Info --------------------------------------------------------------

internal CV_DebugS lnk_debug_s_from_obj(Arena *arena, LNK_Obj *obj);
