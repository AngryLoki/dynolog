// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

// Reordering of these includes can lead to broken builds, so we disable
// formatting.
/* clang-format off */
#include <bpf/vmlinux/vmlinux.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "bperf.h"
/* clang-format off */

#define MAX_CGROUP_LEVELS 10
#define DEFAULT_CGROUP_MAP_SIZE 64

typedef struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
  __uint(key_size, sizeof(__u32));
  __uint(
      value_size,
      sizeof(struct bpf_perf_event_value) * BPERF_MAX_GROUP_SIZE);
  __uint(max_entries, 1);
} value_array;

struct {
  __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
  __uint(key_size, sizeof(__u32));
  __uint(value_size, sizeof(int));
  __uint(max_entries, 1); /* # of cpu, updated before load */
  __uint(map_flags, BPF_F_PRESERVE_ELEMS);
} events SEC(".maps");

value_array diff_readings SEC(".maps");
value_array global_output SEC(".maps");
value_array prev_readings SEC(".maps");

struct {
  __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
  __uint(key_size, sizeof(__u64));
  __uint(
      value_size,
      sizeof(struct bpf_perf_event_value) * BPERF_MAX_GROUP_SIZE);
  __uint(max_entries, DEFAULT_CGROUP_MAP_SIZE);
} cgroup_output SEC(".maps");

int event_cnt = 0;
int cpu_cnt = 0;
int cgroup_update_level = 0;

#define PF_IDLE 0x00000002

static void update_cgroup_output(struct bpf_perf_event_value* diff_val,
                                 struct task_struct *task) {
  struct bpf_perf_event_value* val;
  __u64 id;
  __u32 i;

  id  = bpf_get_current_ancestor_cgroup_id(cgroup_update_level);
  val = bpf_map_lookup_elem(&cgroup_output, &id);
  if (!val)
    return;

  for (i = 0; i < BPERF_MAX_GROUP_SIZE; i++) {
    if (i >= event_cnt)
      break;

    val[i].counter += diff_val[i].counter;
    val[i].enabled += diff_val[i].enabled;
    val[i].running += diff_val[i].running;
  }
}

static __always_inline int bperf_leader_prog(struct task_struct *prev) {
  struct bpf_perf_event_value val, *prev_val, *diff_val, *sys_val;
  __u32 key = bpf_get_smp_processor_id();
  __u32 zero = 0, i;
  long err;

  prev_val = bpf_map_lookup_elem(&prev_readings, &zero);
  if (!prev_val)
    return 0;

  diff_val = bpf_map_lookup_elem(&diff_readings, &zero);
  if (!diff_val)
    return 0;

  sys_val = bpf_map_lookup_elem(&global_output, &zero);
  if (!sys_val)
    return 0;

  for (i = 0; i < BPERF_MAX_GROUP_SIZE; i++) {
    __u32 idx = i * cpu_cnt + key;

    if (i >= event_cnt)
      break;

    err = bpf_perf_event_read_value(&events, idx, &val, sizeof(val));
    if (err)
      continue;

    diff_val[i].counter = val.counter - prev_val[i].counter;
    diff_val[i].enabled = val.enabled - prev_val[i].enabled;
    diff_val[i].running = val.running - prev_val[i].running;
    prev_val[i] = val;

    sys_val[i].counter += diff_val[i].counter;
    sys_val[i].enabled += diff_val[i].enabled;
    sys_val[i].running += diff_val[i].running;
  }

  /* If previous task is idle (PF_IDLE), it means we are switching _from_
   * idle to non-idle task, and current "slice" of counts belongs to idle.
   * It is ok to skip cgroup walk for idle task.
   */
  if (prev->flags & PF_IDLE)
    return 0;

  update_cgroup_output(diff_val, prev);
  return 0;
}

/* This is triggered on context switch */
SEC("tp_btf/sched_switch")
int BPF_PROG(bperf_on_sched_switch, bool preempt, struct task_struct *prev,
            struct task_struct *next) {
  return bperf_leader_prog(prev);
}

/* This program is NOT attached. Instead, this is only triggered by user
 * space via BPF_PROG_TEST_RUN before reading the output. This is need to
 * gather current running data to the output maps.
 * We need a separate program because BPF_PROG_TEST_RUN does not work on
 * tp_btf program (bperf_on_sched_switch). tp_btf program is slightly
 * faster.
 */
SEC("raw_tp/sched_switch")
int BPF_PROG(bperf_read_trigger) {
  /* Account for current task */
  return bperf_leader_prog(bpf_get_current_task_btf());
}

char _license[] SEC("license") = "GPL";
