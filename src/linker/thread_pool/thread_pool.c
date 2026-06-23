// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

internal void
tp_run_tasks(TP_Context *pool, TP_Worker *worker)
{
  for (;;) {
    S64 task_left = ins_atomic_u64_dec_eval(&pool->task_left);

    // are there any tasks left to run?
    if (task_left < 0) {
      break;
    }

    // run task
    Arena *arena   = pool->task_arena ? pool->task_arena->v[worker->id] : 0;
    U64    task_id = pool->task_count - (task_left+1);
    pool->task_func(arena, worker->id, task_id, pool->task_data, pool);

    // cache task count so we dont touch pool memory after atomic inc
    U64 task_count = pool->task_count;

    // on last task ping main thread (main_semaphore is null when worker_count==1,
    // in which case main runs everything inline and never waits)
    U64 task_done = ins_atomic_u64_inc_eval(&pool->task_done);
    if (task_done == task_count && pool->worker_count > 1) {
      semaphore_drop(pool->main_semaphore);
    }
  }
}

internal void
tp_worker_main(void *raw_worker)
{
  TP_Worker  *worker = raw_worker;
  TP_Context *pool   = worker->pool;
  for (; pool->is_live; ) {
    if (semaphore_take(pool->task_semaphore, max_U64)) {
      tp_run_tasks(pool, worker);
    }
  }
}

//
// SHARED (cross-process governor) mode.
//
// Workers 1..worker_count-1 are PARKED on wake_semaphore. They run tasks only
// when woken. Two kinds of wakes:
//   - path A (barrier-free, governor-driven): the worker was woken because the
//     governor acquired a global budget slot for it. When the worker drains
//     (tp_run_tasks returns), it RETURNS that slot: release(budget) + granted--,
//     so the slot can flow to another process mid-pass.
//   - path B (barrier pass): the dispatching thread reserved all worker_count-1
//     slots up front and woke every worker. The worker just runs the pass and
//     re-parks; the dispatcher releases the slots in bulk afterwards. The worker
//     must NOT touch the budget here (cohort must stay live for the barrier).
//
internal void
tp_worker_main_shared(void *raw_worker)
{
  TP_Worker  *worker = raw_worker;
  TP_Context *pool   = worker->pool;
  for (; pool->is_live; ) {
    if (!semaphore_take(pool->wake_semaphore, max_U64)) {
      continue;
    }
    if (!pool->is_live) {
      break;
    }
    // capture pass kind at wake time (only one pass kind is active at once)
    B32 barrier_pass = pool->barrier_pass;

    tp_run_tasks(pool, worker);

    if (!barrier_pass) {
      // path A: hand my budget slot back so another process can use it
      ins_atomic_u64_dec_eval(&pool->granted);
      semaphore_drop(pool->budget_semaphore);
    }
  }
}

//
// Per-process governor. Sleeps until main signals a path-A pass is active, then
// acquires global budget slots (only while THIS process has pending demand) and
// wakes one local parked worker per slot. Slots are returned by the workers
// themselves when they drain, so the governor only ever ACQUIRES.
//
internal void
tp_governor_main(void *raw_pool)
{
  TP_Context *pool = raw_pool;
  for (; pool->is_live; ) {
    // wait for a pass to begin (or for shutdown)
    if (!semaphore_take(pool->governor_semaphore, max_U64)) {
      continue;
    }
    if (!pool->is_live) {
      break;
    }

    // Grant slots while the pass is live and there is demand. Cap total live
    // grants at worker_count-1 (main/worker 0 is the worker_count-th runner and
    // never consumes a slot). `granted` is decremented by workers as they drain.
    for (; ins_atomic_u32_eval(&pool->pass_active); ) {
      S64 task_left = ins_atomic_u64_eval((U64 *)&pool->task_left);
      S64 demand    = task_left > 0 ? task_left : 0;
      S64 cap       = (S64)pool->worker_count - 1;
      S64 live      = ins_atomic_u64_eval((U64 *)&pool->granted);
      S64 want      = Min(cap, demand) - live;

      if (want > 0) {
        // Bounded wait so we re-check pass_active/demand and never block forever
        // on budget that may never free if the pass ends first.
        if (semaphore_take(pool->budget_semaphore, now_time_us() + 1000)) {
          // RE-CHECK pass_active AFTER acquiring the slot. Without this there is a
          // race: the governor can read pass_active==1, get preempted while the
          // main thread ends the pass (pass_active=0) AND finishes its granted==0
          // drain AND starts a path-B barrier pass (which sets barrier_pass=1).
          // A grant issued in that window would wake a worker that reads
          // barrier_pass==1, skip its `granted--`, and corrupt both the path-B
          // cohort and the granted accounting -> permanent hang. Granting only
          // while the pass is still active keeps every path-A grant paired with a
          // worker that drains with barrier_pass==0 before main can return.
          if (ins_atomic_u32_eval(&pool->pass_active)) {
            ins_atomic_u64_inc_eval(&pool->granted);
            semaphore_drop(pool->wake_semaphore);
          } else {
            semaphore_drop(pool->budget_semaphore); // give the slot back; pass ended
          }
        }
      } else {
        // No demand right now (work drained or fully covered). Briefly idle; the
        // pass-end ping will release us promptly via the outer take when we loop.
        sleep_ms(0);
      }
    }
  }
}

internal TP_Context * 
tp_alloc(Arena *arena, U32 worker_count, U32 max_worker_count, String8 name)
{
  ProfBeginDynamic("Alloc Thread Pool [Worker Count: %u]", worker_count);
  AssertAlways(worker_count > 0);

  B32   is_shared = (name.size > 0);
  Temp  scratch   = scratch_begin(&arena, 1);

  // init pool
  TP_Context *pool   = push_array(arena, TP_Context, 1);
  pool->barrier      = barrier_alloc(worker_count);
  pool->is_live      = 1;
  pool->is_shared    = is_shared;
  pool->worker_count = worker_count;
  pool->worker_arr   = push_array(arena, TP_Worker, worker_count);

  // alloc semaphores
  if (worker_count > 1) {
    pool->main_semaphore = semaphore_alloc(0, 1, str8_zero());

    if (is_shared) {
      AssertAlways(worker_count <= max_worker_count);

      // Two NAMED cross-process semaphores. CreateSemaphoreW on an existing name
      // returns the existing object (first process inits with these counts; later
      // processes attach and the supplied counts are ignored by the OS), so all
      // processes share one BUDGET and one BARRIER-LOCK.
      //   BUDGET:       init=max=max_worker_count (the machine core budget).
      //   BARRIER-LOCK: init=max=1 (one full-cohort barrier pass at a time).
      String8 budget_name = push_str8f(scratch.arena, "%S.budget", name);
      String8 block_name  = push_str8f(scratch.arena, "%S.barrierlock", name);
      pool->budget_semaphore       = semaphore_alloc(max_worker_count, max_worker_count, budget_name);
      pool->barrier_lock_semaphore = semaphore_alloc(1, 1, block_name);

      // local wake/governor signalling. governor_semaphore is a 0/1 "at least one
      // pending pass" flag: main pings it with semaphore_drop_if_room (a redundant
      // ping while one is already pending is a harmless no-op, since the pending
      // signal will make the governor re-evaluate the current pass_active anyway).
      pool->wake_semaphore      = semaphore_alloc(0, worker_count, str8_zero());
      pool->governor_semaphore  = semaphore_alloc(0, 1, str8_zero());
    } else {
      // Non-shared mode: EXACTLY as before. 2x headroom on the wake semaphore so
      // a batched ReleaseSemaphore(drop_count) never overflows the max while up
      // to worker_count-1 previously-woken workers have not yet re-taken a permit.
      pool->task_semaphore = semaphore_alloc(0, 2 * worker_count, str8_zero());
    }
  }

  // pick entry point for the workers
  void *worker_entry = is_shared ? tp_worker_main_shared : tp_worker_main;

  // init worker data
  for (U64 i = 0; i < worker_count; i += 1) {
    TP_Worker *worker = &pool->worker_arr[i];
    worker->id        = i;
    worker->pool      = pool;
  }

  // launch worker threads
  for (U64 i = 1; i < worker_count; i += 1) {
    TP_Worker *worker = &pool->worker_arr[i];
    worker->handle    = thread_launch(worker_entry, worker);
  }

  // launch the per-process governor (shared mode only)
  if (is_shared && worker_count > 1) {
    pool->governor_handle = thread_launch(tp_governor_main, pool);
  }

  scratch_end(scratch);
  ProfEnd();
  return pool;
}

internal void
tp_release(TP_Context *pool)
{
  pool->is_live = 0;

  if (pool->is_shared) {
    if (pool->worker_count > 1) {
      // wake governor so it observes !is_live and exits (a pending ping is fine)
      semaphore_drop_if_room(pool->governor_semaphore);
      // wake every parked worker so each observes !is_live and exits. Wakes here
      // are NOT path-A grants (no budget was taken), so mark barrier_pass to keep
      // workers from touching the budget on their way out.
      pool->barrier_pass = 1;
      for (U64 i = 1; i < pool->worker_count; i += 1) {
        semaphore_drop(pool->wake_semaphore);
      }
    }
    for (U64 i = 1; i < pool->worker_count; i += 1) {
      thread_detach(pool->worker_arr[i].handle);
    }
    if (pool->worker_count > 1) {
      thread_detach(pool->governor_handle);
      semaphore_release(pool->budget_semaphore);
      semaphore_release(pool->barrier_lock_semaphore);
      semaphore_release(pool->wake_semaphore);
      semaphore_release(pool->governor_semaphore);
    }
  } else {
    for EachIndex(i, pool->worker_count) {
      semaphore_drop(pool->task_semaphore);
    }
    for (U64 i = 1; i < pool->worker_count; i += 1) {
      thread_detach(pool->worker_arr[i].handle);
    }
    if (pool->worker_count > 1) {
      semaphore_release(pool->task_semaphore);
    }
  }

  barrier_release(pool->barrier);
  if (pool->worker_count > 1) {
    semaphore_release(pool->main_semaphore);
  }

  MemoryZeroStruct(pool);
}

internal TP_Arena *
tp_arena_alloc(TP_Context *pool)
{
  ProfBeginFunction();
  Temp scratch = scratch_begin(0,0);
  Arena **arr = push_array(scratch.arena, Arena *, pool->worker_count);
  for (U64 i = 0; i < pool->worker_count; ++i) {
    arr[i] = arena_alloc("THREAD_POOL");
  }
  Arena **dst = push_array(arr[0], Arena *, pool->worker_count);
  MemoryCopy(dst, arr, sizeof(Arena*) * pool->worker_count);
  TP_Arena *worker_arena_arr = push_array(arr[0], TP_Arena, 1);
  worker_arena_arr->count = pool->worker_count;
  worker_arena_arr->v = dst;
  scratch_end(scratch);
  ProfEnd();
  return worker_arena_arr;
}

internal void
tp_arena_release(TP_Arena **arena_ptr)
{
  ProfBeginFunction();
  for (U64 i = 1; i < (*arena_ptr)->count; ++i) {
    arena_release((*arena_ptr)->v[i]);
  }
  arena_release((*arena_ptr)->v[0]);
  *arena_ptr = NULL;
  ProfEnd();
}

internal TP_Temp
tp_temp_begin(TP_Arena *arena)
{
  ProfBeginFunction();

  Temp first_temp = temp_begin(arena->v[0]);

  TP_Temp temp;
  temp.count = arena->count;
  temp.v     = push_array_no_zero(first_temp.arena, Temp, arena->count);

  temp.v[0] = first_temp;

  for (U64 arena_idx = 1; arena_idx < arena->count; arena_idx += 1) {
    temp.v[arena_idx] = temp_begin(arena->v[arena_idx]);
  }

  ProfEnd();
  return temp;
}

internal void
tp_temp_end(TP_Temp temp)
{
  ProfBeginFunction();
  for (U64 temp_idx = temp.count - 1; temp_idx > 0; temp_idx -= 1) {
    temp_end(temp.v[temp_idx]);
  }
  ProfEnd();
}

internal void
tp_for_parallel_init_state(TP_Context *pool, TP_Arena *task_arena, U64 task_count, TP_TaskFunc *task_func, void *task_data)
{
  pool->task_arena = task_arena;
  pool->task_func  = task_func;
  pool->task_data  = task_data;
  pool->task_count = task_count;
  pool->task_done  = 0;
  ins_atomic_u64_eval_assign(&pool->task_left, task_count);
}

//
// PATH A: barrier-free dispatch (the common case). Pure work-stealing via the
// atomic task_left decrement in tp_run_tasks. Main (worker 0) ALWAYS runs and
// never consumes a global budget slot -> per-process forward-progress guarantee.
// The governor opportunistically borrows budget slots and wakes local parked
// workers; each woken worker returns its slot when it drains.
//
internal void
tp_for_parallel(TP_Context *pool, TP_Arena *task_arena, U64 task_count, TP_TaskFunc *task_func, void *task_data)
{
  if (task_count == 0) {
    return;
  }

  tp_for_parallel_init_state(pool, task_arena, task_count, task_func, task_data);

  if (pool->worker_count == 1) {
    // no workers: main runs everything inline
    tp_run_tasks(pool, &pool->worker_arr[0]);
    return;
  }

  if (pool->is_shared) {
    // announce a path-A pass and let the governor recruit workers as budget frees
    pool->barrier_pass = 0;
    ins_atomic_u32_eval_assign(&pool->pass_active, 1);
    semaphore_drop_if_room(pool->governor_semaphore);

    // main always runs (no slot consumed)
    tp_run_tasks(pool, &pool->worker_arr[0]);

    // all tasks done (last finisher pinged main_semaphore)
    semaphore_take(pool->main_semaphore, max_U64);

    // End the pass so the governor stops issuing new grants.
    ins_atomic_u32_eval_assign(&pool->pass_active, 0);

    // CRITICAL: before returning we must guarantee that no woken worker is still
    // inside tp_run_tasks. Otherwise the next pass's init_state (which resets
    // task_left/task_done) would race a straggler still looping on this pass and
    // corrupt the counters / lose the completion ping -> deadlock.
    //
    // Every governor grant is paired with exactly one wake permit and one worker
    // that, on draining, does `granted--; drop(budget)`. Even grants issued in the
    // tiny window before pass_active was cleared have a pending wake permit that a
    // worker will consume, drain immediately (task_left<0), and account for. So
    // `granted` monotonically drains to 0 once the governor has stopped; spin
    // until it does. This is brief (workers see task_left<0 and exit at once).
    for (; ins_atomic_u64_eval((U64 *)&pool->granted) != 0; ) {
      sleep_ms(0);
    }
  } else {
    // non-shared: EXACTLY as before -- batched wake of drop_count workers.
    U64 drop_count = Min(task_count, pool->worker_count);
    semaphore_drop_n(pool->task_semaphore, (U32)drop_count);
    tp_run_tasks(pool, &pool->worker_arr[0]);
    semaphore_take(pool->main_semaphore, max_U64);
  }
}

//
// PATH B: barrier-pass dispatch. The cohort MUST be exactly worker_count (every
// participant live for the whole pass) so the barrier_wait/broadcast/sum math is
// byte-identical to the non-shared run. In shared mode we reserve the full cohort
// up front: take barrier_lock (only one full-cohort barrier pass globally at a
// time), then acquire worker_count-1 budget slots.
//
// Deadlock-freedom: holding barrier_lock, no OTHER process can be inside a barrier
// pass. Every other process is therefore doing path-A work (which releases budget
// slots as workers drain), or idle, or blocked on barrier_lock holding ZERO budget
// slots. So the worker_count-1 slots we need WILL free up. We hold no other lock.
//
internal void
tp_for_parallel_reserve(TP_Context *pool, TP_Arena *task_arena, U64 task_count, TP_TaskFunc *task_func, void *task_data)
{
  if (task_count == 0) {
    return;
  }

  if (!pool->is_shared || pool->worker_count == 1) {
    // non-shared (or single worker): identical to the plain dispatch
    tp_for_parallel(pool, task_arena, task_count, task_func, task_data);
    return;
  }

  AssertAlways(task_count == pool->worker_count);

  U32 cohort_workers = pool->worker_count - 1; // worker 0 == main is the Nth

  // serialize full-cohort barrier passes across processes
  semaphore_take(pool->barrier_lock_semaphore, max_U64);

  // reserve the whole cohort's budget (blocking, deadlock-free per above)
  semaphore_take_n(pool->budget_semaphore, cohort_workers, max_U64);

  tp_for_parallel_init_state(pool, task_arena, task_count, task_func, task_data);

  // wake the full cohort; these are barrier-pass wakes (workers must not touch
  // budget on drain -- we release in bulk below)
  pool->barrier_pass = 1;
  semaphore_drop_n(pool->wake_semaphore, cohort_workers);

  // main is the worker_count-th participant
  tp_run_tasks(pool, &pool->worker_arr[0]);

  // wait for the cohort to finish
  semaphore_take(pool->main_semaphore, max_U64);

  // release the cohort budget + barrier lock
  semaphore_drop_n(pool->budget_semaphore, cohort_workers);
  semaphore_drop(pool->barrier_lock_semaphore);
}

internal Rng1U64 *
tp_divide_work(Arena *arena, U64 item_count, U32 worker_count)
{
  U64      per_count = CeilIntegerDiv(item_count, worker_count);
  Rng1U64 *range_arr = push_array_no_zero(arena, Rng1U64, worker_count + 1);
  for (U64 i = 0; i < worker_count; i += 1) {
    range_arr[i] = rng_1u64(Min(item_count, i * per_count), 
                            Min(item_count, i * per_count + per_count));
  }

  // thread_pool_dummy_range:
  range_arr[worker_count] = rng_1u64(item_count, item_count);

  return range_arr;
}

internal void
tp_broadcast_(TP_Context *tp, U64 task_id, void *ptr, U64 ptr_size)
{
  if (task_id == 0) {
    tp->broadcast      = ptr;
    tp->broadcast_size = ptr_size;
  }
  barrier_wait(tp->barrier);

  if (task_id != 0) {
    MemoryCopy(ptr, tp->broadcast, tp->broadcast_size);
  }
  barrier_wait(tp->barrier);
}

internal U64
tp_sum_u64(TP_Context *tp, U64 task_id, U64 v)
{
  if (task_id == 0) {
    tp->sum = 0;
  }
  barrier_wait(tp->barrier);

  ins_atomic_u64_add_eval(&tp->sum, v);
  barrier_wait(tp->barrier);

  U64 result = tp->sum;
  barrier_wait(tp->barrier);
  return result;
}

