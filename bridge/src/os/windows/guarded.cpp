// windows/guarded.cpp — os::guarded_call / guard_rearm on Windows.
//
// The ADR amendment "No longjmp out of a vectored handler": longjmp from a
// VEH is unsafe on Windows (it would run frame cleanup from the handler's
// context). Instead the region records a resume point thread-locally and the
// handler REWRITES the faulting thread's CONTEXT to it and returns
// EXCEPTION_CONTINUE_EXECUTION. No C++ unwinding happens: the faulting
// frames are abandoned, exactly as the Linux siglongjmp recovery abandons
// them, so a guarded body must own no resources.
//
// The resume point is captured with RtlCaptureContext (whole-CONTEXT, so the
// stack pointer comes back too). Reaching the capture a second time -- because
// the handler copied the saved CONTEXT into ContextRecord -- is the "a fault
// was recovered" path, distinguished by a volatile thread-local flag, the same
// shape as setjmp's return value but without unwinding.
//
// The handler catches only while a region on THIS thread is active and only
// for memory faults; everything else (and CK3's own crash reporter) sees
// EXCEPTION_CONTINUE_SEARCH. guard_rearm re-adds the handler at the front
// after the engine installs its own, the same race the Linux SIGSEGV re-arm
// handles.

#include "win_internal.h"

#include <mutex>

namespace bridge {
namespace os {

namespace {

constexpr std::size_t kMaxDepth = 8;

struct Frame {
    alignas(16) CONTEXT resume;  // RtlCaptureContext target; 16-byte aligned
    volatile long       faulted; // 0 on the first pass, 1 after the handler
    Fault               fault;
};

struct ThreadGuard {
    Frame       frames[kMaxDepth];
    std::size_t depth = 0;  // number of active frames; frames[depth-1] is innermost
};

thread_local ThreadGuard t_guard;

PVOID g_handler = nullptr;
std::mutex g_handler_mutex;

bool is_memory_fault(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
           code == EXCEPTION_DATATYPE_MISALIGNMENT || code == EXCEPTION_GUARD_PAGE ||
           code == EXCEPTION_STACK_OVERFLOW;
}

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    ThreadGuard& g = t_guard;
    if (g.depth == 0) return EXCEPTION_CONTINUE_SEARCH;  // no guarded region here
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!is_memory_fault(code)) return EXCEPTION_CONTINUE_SEARCH;

    Frame& f = g.frames[g.depth - 1];
    f.fault.code = code;
    // ExceptionInformation[1] is the address for an access violation / in-page error.
    f.fault.address = (ep->ExceptionRecord->NumberParameters >= 2)
                          ? static_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1])
                          : 0;
    f.fault.pc = static_cast<std::uintptr_t>(ep->ContextRecord->Rip);
    f.faulted = 1;
    *ep->ContextRecord = f.resume;  // resume at the capture point, with its Rsp
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool ensure_handler() {
    std::lock_guard<std::mutex> lk(g_handler_mutex);
    if (g_handler) return true;
    g_handler = AddVectoredExceptionHandler(1 /* first */, veh);
    return g_handler != nullptr;
}

}  // namespace

Result guarded_call(GuardedBody body, void* ctx, Fault* fault) {
    if (fault) { fault->code = 0; fault->address = 0; fault->pc = 0; }
    if (!body) return win::failed(ERROR_INVALID_PARAMETER, "body is null");
    if (!ensure_handler()) return win::failed_last("AddVectoredExceptionHandler failed");

    ThreadGuard& g = t_guard;
    if (g.depth >= kMaxDepth)
        return win::failed(ERROR_STACK_OVERFLOW, "guarded regions nested past the fixed depth");

    const std::size_t me = g.depth;
    g.frames[me].faulted = 0;
    g.frames[me].fault = Fault{0, 0, 0};
    ++g.depth;  // the frame is active before the capture

    RtlCaptureContext(&g.frames[me].resume);
    // Reached twice: once now (faulted == 0), and again if the handler rewrote
    // the CONTEXT to here (faulted == 1). Read the volatile thread-local, never
    // a register the restore would have clobbered.
    if (t_guard.frames[me].faulted == 0) {
        body(ctx);
        // Returned without faulting. (If it faulted, control never gets here;
        // it resumes above with faulted == 1.)
        t_guard.depth = me;  // pop
        return win::ok();
    }

    // A fault was recovered. The stack pointer is back to the capture point;
    // the body's frames are gone.
    Fault f = t_guard.frames[me].fault;
    t_guard.depth = me;  // pop
    if (fault) *fault = f;
    return Result{Status::failed, int(f.code), "a memory fault in the guarded body was recovered"};
}

Result guard_rearm() {
    std::lock_guard<std::mutex> lk(g_handler_mutex);
    if (g_handler) { RemoveVectoredExceptionHandler(g_handler); g_handler = nullptr; }
    g_handler = AddVectoredExceptionHandler(1, veh);
    return g_handler ? win::ok() : win::failed_last("AddVectoredExceptionHandler failed");
}

}  // namespace os
}  // namespace bridge
