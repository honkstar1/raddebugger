// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ Names (interned string pool)

internal U32
rgd_name_off(RGD_Builder *b, String8 name)
{
  U64 existing = 0;
  if (hash_table_search_string_u64(b->name_ht, name, &existing)) {
    return (U32)existing;
  }
  // append: U32 len; U8 bytes[len]
  U32     off    = safe_cast_u32(b->names_size);
  U32     len    = safe_cast_u32(name.size);
  String8 record = push_str8_cat(b->arena, str8_struct(&len), name);
  str8_list_push(b->arena, &b->names, record);
  b->names_size += record.size;
  hash_table_push_string_u64(b->arena, b->name_ht, name, off);
  return off;
}

internal String8
rgd_name_from_off(RGD_Parsed *p, U32 off)
{
  if (off + sizeof(U32) > p->names.size) { return str8_zero(); }
  U32 len = *(U32 *)(p->names.str + off);
  if (off + sizeof(U32) + len > p->names.size) { return str8_zero(); }
  return str8(p->names.str + off + sizeof(U32), len);
}

////////////////////////////////
//~ Builder

internal RGD_Builder
rgd_builder_init(Arena *arena, U32 machine, U32 group_id)
{
  RGD_Builder b = {0};
  b.arena    = arena;
  b.machine  = machine;
  b.group_id = group_id;
  b.name_ht  = hash_table_init(arena, 4096);
  // reserve offset 0 as the empty-name sentinel so name_off==0 means "" deterministically
  rgd_name_off(&b, str8_zero());
  return b;
}

internal U64
rgd_builder_push_contrib(RGD_Builder *b, RGD_Contrib contrib, String8 section_bytes)
{
  U64 data_off = b->section_data_size;
  if (section_bytes.size) {
    str8_list_push(b->arena, &b->section_data, push_str8_copy(b->arena, section_bytes));
    b->section_data_size += section_bytes.size;
  }
  contrib.data_off  = section_bytes.size ? data_off : 0;
  contrib.data_size = section_bytes.size;

  RGD_ContribNode *n = push_array(b->arena, RGD_ContribNode, 1);
  n->contrib = contrib;
  SLLQueuePush(b->first_contrib, b->last_contrib, n);
  b->contrib_count += 1;
  return data_off;
}

internal void
rgd_builder_push_reloc(RGD_Builder *b, RGD_Reloc reloc)
{
  RGD_RelocNode *n = push_array(b->arena, RGD_RelocNode, 1);
  n->reloc = reloc;
  SLLQueuePush(b->first_reloc, b->last_reloc, n);
  b->reloc_count += 1;
}

internal void
rgd_builder_push_comdat(RGD_Builder *b, RGD_Comdat comdat)
{
  RGD_ComdatNode *n = push_array(b->arena, RGD_ComdatNode, 1);
  n->comdat = comdat;
  SLLQueuePush(b->first_comdat, b->last_comdat, n);
  b->comdat_count += 1;
}

internal void
rgd_builder_push_directive(RGD_Builder *b, String8 token)
{
  U32     len    = safe_cast_u32(token.size);
  String8 record = push_str8_cat(b->arena, str8_struct(&len), token);
  str8_list_push(b->arena, &b->directives, record);
}

internal U32
rgd_builder_push_sym(RGD_Builder *b, String8 name, RGD_Sym sym)
{
  sym.name_off = rgd_name_off(b, name);
  RGD_SymNode *n = push_array(b->arena, RGD_SymNode, 1);
  n->sym  = sym;
  n->name = name;
  SLLQueuePush(b->first_sym, b->last_sym, n);
  return safe_cast_u32(b->sym_count++);
}

////////////////////////////////
//~ Serialize

typedef struct RGD_SymSort { U64 hash; U32 name_off; U32 push_idx; } RGD_SymSort; // 16B (radsort-friendly)

internal int
rgd_sym_sort_is_before(void *raw_a, void *raw_b)
{
  RGD_SymSort *a = raw_a, *b = raw_b;
  if (a->hash != b->hash) { return a->hash < b->hash; }
  return a->name_off < b->name_off; // deterministic tie-break
}

internal String8List
rgd_serialize(Arena *arena, RGD_Builder *b)
{
  // --- collect symbols in push order; hash-sort via index pairs; build push->sorted perm ---
  U64         sym_count     = b->sym_count;
  RGD_Sym    *syms_unsorted = push_array(arena, RGD_Sym, sym_count);
  RGD_SymSort *sort         = push_array(arena, RGD_SymSort, sym_count);
  {
    U64 i = 0;
    for (RGD_SymNode *n = b->first_sym; n != 0; n = n->next, i += 1) {
      syms_unsorted[i] = n->sym;
      sort[i].hash     = n->sym.trie_hash;
      sort[i].name_off = n->sym.name_off;
      sort[i].push_idx = safe_cast_u32(i);
    }
  }
  radsort(sort, sym_count, rgd_sym_sort_is_before);
  RGD_Sym *syms = push_array(arena, RGD_Sym, sym_count);
  U32     *perm = push_array(arena, U32, sym_count); // perm[push_idx] = sorted position
  for EachIndex(k, sym_count) {
    syms[k]               = syms_unsorted[sort[k].push_idx];
    perm[sort[k].push_idx] = safe_cast_u32(k);
  }

  // --- collect relocs; remap _Boundary targets from push-order to sorted BoundarySyms index ---
  U64        reloc_count = b->reloc_count;
  RGD_Reloc *relocs      = push_array(arena, RGD_Reloc, reloc_count);
  {
    U64 i = 0;
    for (RGD_RelocNode *n = b->first_reloc; n != 0; n = n->next, i += 1) {
      relocs[i] = n->reloc;
      if (relocs[i].target_kind == RGD_RelocTarget_Boundary && relocs[i].target_boundary_sym != RGD_BAD_IDX) {
        relocs[i].target_boundary_sym = perm[relocs[i].target_boundary_sym];
      }
    }
  }

  // --- collect contribs (already in push order = (obj_idx, obj_sect_idx)) ---
  U64          contrib_count = b->contrib_count;
  RGD_Contrib *contribs      = push_array(arena, RGD_Contrib, contrib_count);
  {
    U64 i = 0;
    for (RGD_ContribNode *n = b->first_contrib; n != 0; n = n->next, i += 1) { contribs[i] = n->contrib; }
  }

  // --- collect comdats (push order) ---
  U64         comdat_count = b->comdat_count;
  RGD_Comdat *comdats      = push_array(arena, RGD_Comdat, comdat_count);
  {
    U64 i = 0;
    for (RGD_ComdatNode *n = b->first_comdat; n != 0; n = n->next, i += 1) { comdats[i] = n->comdat; }
  }

  // --- per-stream byte blobs ---
  String8 stream_bytes[RGD_Stream_COUNT] = {0};
  stream_bytes[RGD_Stream_Names]          = str8_list_join(arena, &b->names, 0);
  stream_bytes[RGD_Stream_BoundarySyms]   = str8_array(syms, sym_count);
  stream_bytes[RGD_Stream_Comdats]        = str8_array(comdats, comdat_count);
  stream_bytes[RGD_Stream_Contribs]       = str8_array(contribs, contrib_count);
  stream_bytes[RGD_Stream_SectionData]    = str8_list_join(arena, &b->section_data, 0);
  stream_bytes[RGD_Stream_DeferredRelocs] = str8_array(relocs, reloc_count);
  stream_bytes[RGD_Stream_Directives]     = str8_list_join(arena, &b->directives, 0);

  // --- header + page-aligned stream directory ---
  RGD_Header *header = push_array(arena, RGD_Header, 1);
  MemoryCopy(header->magic, RGD_MAGIC, sizeof(header->magic));
  header->version           = RGD_VERSION;
  header->machine           = b->machine;
  header->group_id          = b->group_id;
  header->source_obj_count  = b->source_obj_count;
  header->boundary_sym_count = safe_cast_u32(sym_count);
  header->comdat_count       = safe_cast_u32(comdat_count);
  header->type_index_base    = CV_MinComplexTypeIndex;

  U64 off = AlignPow2(sizeof(RGD_Header), RGD_PAGE_SIZE);
  for (U64 s = 0; s < RGD_Stream_COUNT; s += 1) {
    header->streams[s].off  = off;
    header->streams[s].size = stream_bytes[s].size;
    off += AlignPow2(stream_bytes[s].size, RGD_PAGE_SIZE);
  }

  // --- emit: header, then each stream at its page-aligned offset ---
  String8List out = {0};
  str8_list_push(arena, &out, str8_struct(header));
  {
    U64 pad = AlignPow2(sizeof(RGD_Header), RGD_PAGE_SIZE) - sizeof(RGD_Header);
    str8_list_push(arena, &out, str8(push_array(arena, U8, pad), pad));
  }
  for (U64 s = 0; s < RGD_Stream_COUNT; s += 1) {
    str8_list_push(arena, &out, stream_bytes[s]);
    U64 pad = AlignPow2(stream_bytes[s].size, RGD_PAGE_SIZE) - stream_bytes[s].size;
    if (pad) { str8_list_push(arena, &out, str8(push_array(arena, U8, pad), pad)); }
  }

  return out;
}

////////////////////////////////
//~ Parse (over a read-only mapping)

internal B32
rgd_is_valid(RGD_Parsed *p)
{
  return p->header != 0;
}

internal RGD_Parsed
rgd_parse(String8 data)
{
  RGD_Parsed p = {0};
  p.data = data;

  if (data.size < sizeof(RGD_Header)) { return p; }
  RGD_Header *header = (RGD_Header *)data.str;
  if (MemoryCompare(header->magic, RGD_MAGIC, sizeof(header->magic)) != 0) { return p; }
  if (header->version != RGD_VERSION) { return p; }

  // bounds-check every stream
  for (U64 s = 0; s < RGD_Stream_COUNT; s += 1) {
    if (header->streams[s].off + header->streams[s].size > data.size) { return p; }
  }

  p.header = header;

  RGD_StreamDir *dir = header->streams;
  p.names        = str8(data.str + dir[RGD_Stream_Names].off,        dir[RGD_Stream_Names].size);
  p.syms         = (RGD_Sym     *)(data.str + dir[RGD_Stream_BoundarySyms].off);
  p.comdats      = (RGD_Comdat  *)(data.str + dir[RGD_Stream_Comdats].off);
  p.sections     = (RGD_Section *)(data.str + dir[RGD_Stream_Sections].off);
  p.contribs     = (RGD_Contrib *)(data.str + dir[RGD_Stream_Contribs].off);
  p.contrib_count = dir[RGD_Stream_Contribs].size / sizeof(RGD_Contrib);
  p.section_data = str8(data.str + dir[RGD_Stream_SectionData].off, dir[RGD_Stream_SectionData].size);
  p.relocs       = (RGD_Reloc   *)(data.str + dir[RGD_Stream_DeferredRelocs].off);
  p.reloc_count  = dir[RGD_Stream_DeferredRelocs].size / sizeof(RGD_Reloc);
  p.directives   = str8(data.str + dir[RGD_Stream_Directives].off, dir[RGD_Stream_Directives].size);

  return p;
}
