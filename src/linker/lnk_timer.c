// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

global LNK_Timer g_timers[LNK_Timer_Count];

internal void
lnk_timer_begin(LNK_TimerType timer)
{
  g_timers[timer].begin = now_time_us();
}

internal void
lnk_timer_end(LNK_TimerType timer)
{
  g_timers[timer].end = now_time_us();
}

global U64 g_summary_phase_us   [LNK_SummaryPhase_Count];
global U64 g_summary_phase_start[LNK_SummaryPhase_Count];

internal void
lnk_summary_phase_begin(LNK_SummaryPhase phase)
{
  g_summary_phase_start[phase] = now_time_us();
}

internal void
lnk_summary_phase_end(LNK_SummaryPhase phase)
{
  // atomic: the Write bracket runs on the background image-write thread
  ins_atomic_u64_add_eval(&g_summary_phase_us[phase], now_time_us() - g_summary_phase_start[phase]);
}

internal String8
lnk_string_from_timer_type(LNK_TimerType type)
{
  switch (type) {
  case LNK_Timer_Image: return str8_lit("Image");
  case LNK_Timer_Pdb:   return str8_lit("PDB");
  case LNK_Timer_Rdi:   return str8_lit("RDI");
  case LNK_Timer_Lib:   return str8_lit("Lib");
  case LNK_Timer_Debug: return str8_lit("Debug");
  default: InvalidPath;
  }
  return str8_zero();
}

