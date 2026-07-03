// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

typedef enum LNK_TimerType
{
  LNK_Timer_Image,
  LNK_Timer_Pdb,
  LNK_Timer_Rdi,
  LNK_Timer_Lib,
  LNK_Timer_Debug,
  LNK_Timer_Count
} LNK_TimerType;

typedef struct LNK_Timer
{
  U64 begin;
  U64 end;
} LNK_Timer;

internal void lnk_timer_begin(LNK_TimerType timer);
internal void lnk_timer_end(LNK_TimerType timer);

// Phase-wall accumulators for the end-of-link summary line. Unlike LNK_Timer
// (single begin/end shot), these ACCUMULATE across repeated brackets (e.g.
// lnk_load_inputs runs once per input round). Always measured -- two QPC
// stamps per bracket cost nothing. Image/Debug/PDB/RDI buckets come from
// g_timers; these cover the phases that had no timer.
typedef enum LNK_SummaryPhase
{
  LNK_SummaryPhase_Input,   // lnk_load_inputs (parse/load objs+libs), all rounds
  LNK_SummaryPhase_Resolve, // lnk_link_inputs minus contained Input time (lib search + member resolution)
  LNK_SummaryPhase_Icf,     // lnk_opt_icf
  LNK_SummaryPhase_Ref,     // lnk_opt_ref
  LNK_SummaryPhase_Write,   // image write thread (overlaps debug info)
  LNK_SummaryPhase_Count
} LNK_SummaryPhase;

internal void lnk_summary_phase_begin(LNK_SummaryPhase phase);
internal void lnk_summary_phase_end(LNK_SummaryPhase phase);

