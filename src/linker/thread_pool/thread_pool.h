// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

#pragma once

struct TP_Context;
#define THREAD_POOL_TASK_FUNC(name) void name(Arena *arena, U64 worker_id, U64 task_id, void *raw_task, struct TP_Context *tp)
typedef THREAD_POOL_TASK_FUNC(TP_TaskFunc);

typedef struct TP_Arena
{
  U64     count;
  Arena **v;
} TP_Arena;

typedef struct TP_Temp
{
  U64   count;
  Temp *v;
} TP_Temp;

typedef struct TP_Worker
{
  U64                id;
  struct TP_Context *pool;
  Thread             handle;
} TP_Worker;

typedef struct TP_Context
{
  B32          is_live;
  Semaphore    exec_semaphore;
  Semaphore    task_semaphore;
  Semaphore    main_semaphore;
  Barrier      barrier;
  void        *broadcast;
  U64          broadcast_size;
  U64          sum;

  // shared (cross-process) governor mode; all zero in non-shared mode
  B32          is_shared;
  Semaphore    budget_semaphore;       // NAMED: global core budget; init=max=max_worker_count
  Semaphore    barrier_lock_semaphore; // NAMED: serializes full-cohort barrier passes; init=max=1
  Semaphore    wake_semaphore;         // local: governor/dispatcher wakes one parked worker per drop
  Semaphore    governor_semaphore;     // local: main pings governor that a path-A pass is active
  Thread       governor_handle;
  volatile U32 pass_active;            // 1 while a path-A (barrier-free) pass is in flight
  volatile U32 barrier_pass;           // 1 while the current wake cohort is a path-B barrier pass
  volatile S64 granted;                // budget slots currently held by woken path-A workers

  U32          worker_count;
  TP_Worker   *worker_arr;

  TP_Arena    *task_arena;
  TP_TaskFunc *task_func;
  void        *task_data;
  U64          task_count;
  U64          task_done;
  S64          task_left;
} TP_Context;

internal TP_Context * tp_alloc(Arena *arena, U32 worker_count, U32 max_worker_count, String8 name);
internal void         tp_release(TP_Context *pool);
internal TP_Arena *   tp_arena_alloc(TP_Context *pool);
internal void         tp_arena_release(TP_Arena **arena_ptr);
internal TP_Temp      tp_temp_begin(TP_Arena *arena);
internal void         tp_temp_end(TP_Temp temp);
#define tp_for_parallel_prof(pool, arena, task_count, task_func, task_data, zone_name) ProfBegin(zone_name); tp_for_parallel(pool, arena, task_count, task_func, task_data); ProfEnd();
internal void         tp_for_parallel(TP_Context *pool, TP_Arena *arena, U64 task_count, TP_TaskFunc *task_func, void *task_data);
// Barrier-pass dispatch: task_func uses barrier_wait/tp_broadcast/tp_sum_u64, so the
// cohort MUST be exactly worker_count. In shared mode this reserves the full cohort
// (barrier_lock + worker_count-1 budget slots) before running; in non-shared mode it is
// identical to tp_for_parallel.
internal void         tp_for_parallel_reserve(TP_Context *pool, TP_Arena *arena, U64 task_count, TP_TaskFunc *task_func, void *task_data);
#define tp_for_parallel_reserve_prof(pool, arena, task_count, task_func, task_data, zone_name) ProfBegin(zone_name); tp_for_parallel_reserve(pool, arena, task_count, task_func, task_data); ProfEnd();
internal Rng1U64 *    tp_divide_work(Arena *arena, U64 item_count, U32 worker_count);
#define tp_broadcast(p) tp_broadcast_(tp, task_id, p, sizeof(*p))

