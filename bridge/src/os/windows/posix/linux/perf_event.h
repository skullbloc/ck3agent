#pragma once
// Windows POSIX shim (ADR 0001 P5), kind: type. The struct and constants the
// CK3_AI_WATCH diagnostic (include/ai_enabled_watch.h) fills in; the syscall
// that would take them, syscall(__NR_perf_event_open), refuses with ENOSYS, so
// the diagnostic reports its existing "watchpoint open failed" path.
#include "../ck3_posix.h"
#define PERF_TYPE_BREAKPOINT 5
struct perf_event_attr {
    unsigned int type, size;
    unsigned long long config, sample_period, sample_type, read_format;
    unsigned long long disabled : 1, inherit : 1, pinned : 1, exclusive : 1,
        exclude_user : 1, exclude_kernel : 1, exclude_hv : 1, exclude_idle : 1,
        mmap : 1, comm : 1, freq : 1, inherit_stat : 1, enable_on_exec : 1,
        task : 1, watermark : 1, precise_ip : 2, rest : 47;
    unsigned int wakeup_events, bp_type;
    unsigned long long bp_addr, bp_len;
};
