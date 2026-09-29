/*
 * win32_compat.c - POSIX implementation of generic Win32 host primitives.
 *
 * See win32_compat.h. This is deliberately a *generic* OS-primitive layer
 * (threads/events/mutexes/atomics/heap/timers) -- it carries no Xbox
 * semantics. The Xbox kernel HLE in src/kernel builds on top of it.
 *
 * POSIX (Linux/MacOS) only.
 */

#if !defined(_WIN32)

/* Enable memfd_create, MAP_FIXED_NOREPLACE, timegm. Must precede all #includes. */
#define _GNU_SOURCE
/* Darwin: exposes memset_s, its explicit_bzero equivalent. */
#define __STDC_WANT_LIB_EXT1__ 1

#include "win32_compat.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <fenv.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <link.h>        /* dl_iterate_phdr: our own code segment */
#include <dlfcn.h>       /* dladdr: where a thread the freeze cannot park is */
#include <ucontext.h>    /* the interrupted PC in the suspend/freeze handlers */
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#else
#include <sys/sysinfo.h>
#endif

/* ---- Android (Bionic) compatibility ----------------------------------
 * Two generic POSIX primitives this file uses are missing on Bionic:
 *   - pthread_cancel: Bionic omits it by design. Its only caller here is
 *     TerminateThread, which marks the thread object exited/signaled right
 *     after this returns, so a best-effort no-op is safe for bring-up.
 *   - explicit_bzero: not exposed through the headers we include on Bionic;
 *     provide a compiler-barrier memset equivalent.
 * Added for the aarch64/Android (NDK) port -- see docs/blinx-apk-notes.md. */
#if defined(__ANDROID__)
static inline void recomp_explicit_bzero(void *p, size_t n) {
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) *q++ = 0;
}
#define explicit_bzero(p, n) recomp_explicit_bzero((p), (n))
static inline int pthread_cancel(pthread_t t) { (void)t; return 0; }
#endif /* __ANDROID__ */

/* ===================================================================== */
/* Last-error (thread-local)                                             */
/* ===================================================================== */

static __thread DWORD t_last_error = 0;

DWORD GetLastError(void)            { return t_last_error; }
VOID  SetLastError(DWORD code)      { t_last_error = code; }

/* ===================================================================== */
/* Interlocked atomics                                                   */
/* ===================================================================== */

LONG InterlockedIncrement(volatile LONG *p)        { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(volatile LONG *p)        { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

LONG InterlockedCompareExchange(volatile LONG *p, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG xchg, LONGLONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID xchg, PVOID cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

/* ===================================================================== */
/* Critical sections (recursive pthread mutex)                           */
/* ===================================================================== */

VOID InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->LockSemaphore = m;
}

VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
}

VOID EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
    cs->RecursionCount++;
}

VOID LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) return;
    cs->RecursionCount--;
    pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) == 0) {
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

VOID DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->LockSemaphore) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
        free(cs->LockSemaphore);
        cs->LockSemaphore = NULL;
    }
}

/* ===================================================================== */
/* Slim reader/writer locks                                              */
/* ===================================================================== */

/* An SRWLOCK is usable straight from SRWLOCK_INIT, so the pthread_rwlock_t
 * behind it has to appear on first use. Unlike the condition variables below
 * -- whose lazy init is covered by the caller holding the paired CRITICAL
 * SECTION -- an SRWLOCK is by definition taken from several threads at once
 * with nothing else held, so first use genuinely races. Serialise just that:
 * once Ptr is published, every acquire is a plain atomic load. */
static pthread_rwlock_t *srw_lazy_init(PSRWLOCK lock)
{
    pthread_rwlock_t *rw = __atomic_load_n((pthread_rwlock_t **)&lock->Ptr,
                                           __ATOMIC_ACQUIRE);
    if (!rw) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        rw = (pthread_rwlock_t *)lock->Ptr;
        if (!rw) {
            rw = (pthread_rwlock_t *)malloc(sizeof(*rw));
            pthread_rwlock_init(rw, NULL);
            __atomic_store_n((pthread_rwlock_t **)&lock->Ptr, rw, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return rw;
}

VOID InitializeSRWLock(PSRWLOCK lock)
{
    lock->Ptr = NULL;
    srw_lazy_init(lock);
}

VOID AcquireSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_rdlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_unlock(srw_lazy_init(lock)); }
VOID AcquireSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_wrlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_unlock(srw_lazy_init(lock)); }

/* ===================================================================== */
/* One-time initialisation                                               */
/* ===================================================================== */

/* Win32 semantics: the callback runs at most once for a given INIT_ONCE, and
 * a callback returning FALSE leaves it un-run so a later call retries. Ptr
 * doubles as the "done" flag. One global mutex covers every INIT_ONCE --
 * initialisation is rare, and the fast path never touches it. */
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, PVOID *context)
{
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;

    if (__atomic_load_n(&once->Ptr, __ATOMIC_ACQUIRE))
        return TRUE;

    pthread_mutex_lock(&once_lock);
    BOOL ok = TRUE;
    if (!once->Ptr) {
        ok = fn(once, param, context);
        if (ok)
            __atomic_store_n(&once->Ptr, (PVOID)1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    return ok;
}

/* ===================================================================== */
/* Condition variables (paired with a CRITICAL_SECTION)                  */
/* ===================================================================== */

/* Forward decl; the definition lives further down with the wait helpers. */
static void deadline_from_ms(DWORD ms, struct timespec *ts);

static void cv_lazy_init(PCONDITION_VARIABLE cv)
{
    if (!cv->Ptr) {
        pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
        pthread_cond_init(c, NULL);
        /* Race window OK for typical Win32 usage (the CS is held). */
        cv->Ptr = c;
    }
}

VOID InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv->Ptr = NULL;
    cv_lazy_init(cv);
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    cv_lazy_init(cv);
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_cond_t  *c = (pthread_cond_t  *)cv->Ptr;
    pthread_mutex_t *m = (pthread_mutex_t *)cs->LockSemaphore;
    if (ms == INFINITE) {
        pthread_cond_wait(c, m);
        return TRUE;
    }
    struct timespec ts;
    deadline_from_ms(ms, &ts);
    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc == ETIMEDOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}

VOID WakeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_signal((pthread_cond_t *)cv->Ptr);
}

VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_broadcast((pthread_cond_t *)cv->Ptr);
}

/* ===================================================================== */
/* Waitable kernel objects                                               */
/* ===================================================================== */

typedef enum { K_EVENT, K_SEM, K_MUTEX, K_THREAD, K_TIMER, K_HEAP,
               K_FILEMAP, K_FILE, K_WAITABLE_TIMER } w32_kind;

#define W32_MAX_APC 16

typedef struct w32_object {
    w32_kind        kind;
    LONG            refcount;
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    /* event */
    int             signaled;
    int             manual_reset;

    /* semaphore */
    long            sem_count;
    long            sem_max;

    /* mutex */
    DWORD           mtx_owner;
    int             mtx_recursion;

    /* thread */
    pthread_t       thread;
    int             thread_joinable;
    DWORD           tid;
    int             exited;
    DWORD           exit_code;
    int             suspend_count;
    pthread_cond_t  gate;
    /* Cross-thread (mid-run) suspend via signals -- see SuspendThread. Async
     * flags only: sig_park_req mirrors "suspend_count > 0", sig_parked tells the
     * suspender the target is actually parked in the handler. */
    volatile sig_atomic_t sig_park_req;
    volatile sig_atomic_t sig_parked;
    /* Parked by a host freeze (xbox_HostFreeze). */
    volatile sig_atomic_t host_frozen;
    /* A thread the freeze cannot park because it keeps running outside this
     * library: where the last freeze signal found it (pc, lr and a few frame
     * pointer return addresses), how long it has been busy there, and whether
     * the freeze controller has decided to park it anyway (freeze_ctl_thread). */
    volatile uintptr_t freeze_pc, freeze_lr, freeze_bt[6];
    volatile sig_atomic_t force_park;
    int             busy_ticks;
    int64_t         cpu_ns_last;
    pid_t           os_tid;
    uintptr_t       stk_lo, stk_hi;
    /* Depth of guest kernel calls in progress (xbox_ThreadKernelEnter). */
    volatile int    in_kcall;
    LPTHREAD_START_ROUTINE start;
    LPVOID          start_param;
    int             priority;
    PAPCFUNC        apc_func[W32_MAX_APC];
    ULONG_PTR       apc_data[W32_MAX_APC];
    int             apc_count;

    /* timer-queue timer */
    int             timer_cancel;
    DWORD           timer_due;
    DWORD           timer_period;
    WAITORTIMERCALLBACK timer_cb;
    PVOID           timer_param;

    /* waitable timer */
    int             waitable_manual_reset;
    struct timespec waitable_due_time;
    int             waitable_triggered;
    int             waitable_armed;

    /* file mapping / fd-backed file handle */
    int             fd;
    SIZE_T          map_size;
    char           *file_path;
    /* A game file inside the APK (kernel_zipfs.c): bytes [slice_base,
     * slice_base + slice_size) of fd. slice_kind 1 = that file, 2 = a directory
     * that exists only in the archive, 0 = fd is the file itself. */
    int             slice_kind;
    int64_t         slice_base;
    int64_t         slice_size;
} w32_object;

/* pseudo handles for "current thread"/"current process" */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)(LONG_PTR)-2)
#define STILL_ACTIVE 259u

static __thread w32_object *t_self_obj = NULL;
static __thread DWORD       t_tid      = 0;
static volatile LONG        s_next_tid = 1000;

DWORD GetCurrentThreadId(void)
{
    if (t_tid == 0)
        t_tid = (DWORD)InterlockedIncrement(&s_next_tid);
    return t_tid;
}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentThread(void)   { return t_self_obj ? (HANDLE)t_self_obj : PSEUDO_CURRENT_THREAD; }
HANDLE GetCurrentProcess(void)  { return PSEUDO_CURRENT_PROCESS; }

static w32_object *obj_alloc(w32_kind kind)
{
    w32_object *o = (w32_object *)calloc(1, sizeof(w32_object));
    o->kind     = kind;
    o->refcount = 1;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
    pthread_cond_init(&o->gate, NULL);
    return o;
}

static void obj_release(w32_object *o)
{
    if (InterlockedDecrement(&o->refcount) > 0)
        return;
    if (o->kind == K_FILE) {
        if (o->fd >= 0) close(o->fd);
        free(o->file_path);
    } else if (o->kind == K_FILEMAP) {
        if (o->fd >= 0) close(o->fd);
    } else if (o->kind == K_WAITABLE_TIMER) {
        /* Waitable timers: no special cleanup needed */
    }
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    pthread_cond_destroy(&o->gate);
    free(o);
}

/* ---- fd-backed file handle (for the file-I/O HLE) -------------------- */
HANDLE w32_open_handle(int fd, const char *host_path)
{
    w32_object *o = obj_alloc(K_FILE);
    o->fd        = fd;
    o->file_path = host_path ? strdup(host_path) : NULL;
    return (HANDLE)o;
}

HANDLE w32_open_slice_handle(int fd, const char *host_path, int kind,
                             int64_t base, int64_t size)
{
    w32_object *o = (w32_object *)w32_open_handle(fd, host_path);
    o->slice_kind = kind;
    o->slice_base = base;
    o->slice_size = size;
    return (HANDLE)o;
}

int w32_handle_slice(HANDLE h, int64_t *base, int64_t *size)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_FILE || !o->slice_kind) {
        if (base) *base = 0;
        if (size) *size = 0;
        return 0;
    }
    if (base) *base = o->slice_base;
    if (size) *size = o->slice_size;
    return o->slice_kind;
}

int w32_handle_fd(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    return (o && o->kind == K_FILE) ? o->fd : -1;
}

const char *w32_handle_path(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    return (o && o->kind == K_FILE) ? o->file_path : NULL;
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS ||
        h == INVALID_HANDLE_VALUE)
        return TRUE;
    obj_release((w32_object *)h);
    return TRUE;
}

BOOL DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc, PHANDLE dst,
                     DWORD access, BOOL inherit, DWORD options)
{
    (void)srcProc; (void)dstProc; (void)access; (void)inherit;
    if (!dst) return FALSE;
    if (src == PSEUDO_CURRENT_THREAD)  src = GetCurrentThread();
    if (src == PSEUDO_CURRENT_PROCESS) { *dst = src; return TRUE; }
    if (src == PSEUDO_CURRENT_THREAD || !src) { *dst = src; return TRUE; }
    w32_object *o = (w32_object *)src;
    InterlockedIncrement(&o->refcount);
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        obj_release(o);
    return TRUE;
}

/* ---- deadline helper -------------------------------------------------- */
static void deadline_from_ms(DWORD ms, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Run any pending user-APCs for the calling thread. Returns count run. */
static int drain_apcs(void)
{
    w32_object *o = t_self_obj;
    int run = 0;
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    while (o->apc_count > 0) {
        PAPCFUNC  f = o->apc_func[0];
        ULONG_PTR d = o->apc_data[0];
        memmove(o->apc_func, o->apc_func + 1, sizeof(PAPCFUNC) * (o->apc_count - 1));
        memmove(o->apc_data, o->apc_data + 1, sizeof(ULONG_PTR) * (o->apc_count - 1));
        o->apc_count--;
        pthread_mutex_unlock(&o->lock);
        f(d);
        run++;
        pthread_mutex_lock(&o->lock);
    }
    pthread_mutex_unlock(&o->lock);
    return run;
}

static int timespec_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec != b->tv_sec ? a->tv_sec < b->tv_sec : a->tv_nsec < b->tv_nsec;
}

/* Signalled once the due time passes; latched, so a manual-reset timer stays
 * signalled until it is set or cancelled again. Caller holds o->lock. */
static int waitable_due(w32_object *o)
{
    struct timespec now;
    if (o->waitable_triggered) return 1;
    if (!o->waitable_armed) return 0;
    clock_gettime(CLOCK_REALTIME, &now);
    if (timespec_before(&now, &o->waitable_due_time)) return 0;
    o->waitable_triggered = 1;
    return 1;
}

/*
 * Wait on a single object. The object lock must NOT be held.
 * Returns WAIT_OBJECT_0 / WAIT_TIMEOUT.
 */
static DWORD wait_single(w32_object *o, DWORD ms)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    pthread_mutex_lock(&o->lock);
    DWORD result = WAIT_OBJECT_0;

    int logged_block = 0;
    for (;;) {
        int ready = 0;
        switch (o->kind) {
        case K_EVENT:  ready = o->signaled; break;
        case K_THREAD: ready = o->exited;   break;
        case K_SEM:    ready = (o->sem_count > 0); break;
        case K_MUTEX:
            ready = (o->mtx_owner == 0 || o->mtx_owner == GetCurrentThreadId());
            break;
        case K_WAITABLE_TIMER: ready = waitable_due(o); break;
        default:       ready = 1; break;
        }
        if (ready) break;

        /* Diagnostic (RECOMP_WAIT_LOG): the actual host PARK point for every
         * backed wait — Ke, Nt, or raw Win32 all funnel here. Logged once per
         * wait, with the real gettid(), so a thread that hangs shows exactly
         * which object+kind it blocked on (complements the Ke-only KEDIAG at the
         * bridge entry). Silence here for a hung thread => it is NOT a backed
         * host wait (a spin-poll or a critical-section pthread_mutex instead). */
        if (!logged_block && getenv("RECOMP_WAIT_LOG")) {
            logged_block = 1;
            fprintf(stderr, "  [WAITLOG] BLOCK tid=%d obj=%p kind=%d ms=%d "
                    "ev_sig=%d sem=%d mtx_own=%u\n",
                    gettid(), (void *)o, (int)o->kind, (int)ms,
                    o->signaled, o->sem_count, (unsigned)o->mtx_owner);
            fflush(stderr);
        }

        /* An armed timer has its own deadline. Waiting on the caller's alone
         * would sleep straight past the due time, so take whichever comes
         * first and re-test. */
        struct timespec until = ts;
        int bounded = timed;
        if (o->kind == K_WAITABLE_TIMER && o->waitable_armed &&
            (!timed || timespec_before(&o->waitable_due_time, &ts))) {
            until = o->waitable_due_time;
            bounded = 1;
        }
        int rc = bounded ? pthread_cond_timedwait(&o->cond, &o->lock, &until)
                         : pthread_cond_wait(&o->cond, &o->lock);
        if (rc == ETIMEDOUT && timed && !timespec_before(&until, &ts)) {
            result = WAIT_TIMEOUT; break;
        }
    }

    if (result == WAIT_OBJECT_0) {
        switch (o->kind) {
        case K_EVENT: if (!o->manual_reset) o->signaled = 0; break;
        case K_WAITABLE_TIMER:
            if (!o->waitable_manual_reset) { o->waitable_triggered = 0; o->waitable_armed = 0; }
            break;
        case K_SEM:   o->sem_count--; break;
        case K_MUTEX: o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion++; break;
        default: break;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return WAIT_OBJECT_0;
    return wait_single((w32_object *)h, ms);
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return WaitForSingleObject(h, ms);
}

/*
 * WaitForMultipleObjects: polling implementation. Adequate for the light
 * multi-object waits the Xbox kernel HLE issues; not a high-throughput path.
 */
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, waitAll, ms, FALSE);
}

DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    for (;;) {
        if (alertable && drain_apcs() > 0)
            return WAIT_IO_COMPLETION;

        if (waitAll) {
            DWORD got = 0;
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) got++;
            if (got == count) return WAIT_OBJECT_0;
        } else {
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)
                    return WAIT_OBJECT_0 + i;
        }

        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > ts.tv_sec ||
                (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))
                return WAIT_TIMEOUT;
        }
        usleep(1000);
    }
}

/* ===================================================================== */
/* Events                                                                */
/* ===================================================================== */

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_EVENT);
    o->manual_reset = manualReset ? 1 : 0;
    o->signaled     = initialState ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    (void)name;
    return CreateEventA(sa, manualReset, initialState, NULL);
}

BOOL SetEvent(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL PulseEvent(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Semaphores                                                            */
/* ===================================================================== */

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_SEM);
    o->sem_count = initial;
    o->sem_max   = maximum;
    return (HANDLE)o;
}
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name)
{
    (void)name;
    return CreateSemaphoreA(sa, initial, maximum, NULL);
}

BOOL ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_SEM) return FALSE;
    pthread_mutex_lock(&o->lock);
    if (previousCount) *previousCount = (LONG)o->sem_count;
    o->sem_count += releaseCount;
    if (o->sem_max && o->sem_count > o->sem_max) o->sem_count = o->sem_max;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Mutexes                                                               */
/* ===================================================================== */

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_MUTEX);
    if (initialOwner) { o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion = 1; }
    return (HANDLE)o;
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name)
{
    (void)name;
    return CreateMutexA(sa, initialOwner, NULL);
}

BOOL ReleaseMutex(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_MUTEX) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&o->lock);
    if (o->mtx_owner != GetCurrentThreadId()) {
        pthread_mutex_unlock(&o->lock);
        SetLastError(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--o->mtx_recursion <= 0) {
        o->mtx_owner = 0;
        o->mtx_recursion = 0;
        pthread_cond_broadcast(&o->cond);
    }
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Threads                                                               */
/* ===================================================================== */

/* Real cross-thread (mid-run) suspension on POSIX, via signals.
 *
 * The Xbox thread pool parks its worker threads by having the MAIN thread call
 * SuspendThread(worker) and later ResumeThread(worker) -- cross-thread. POSIX
 * has no primitive to pause an arbitrary running thread, so this was a no-op,
 * and the workers ran to completion and terminated instead of parking for
 * reuse; the intro then had no live worker to service its item and hung. The
 * standard technique: a dedicated signal whose handler, running IN the target,
 * blocks in sigsuspend until a resume signal wakes it. RESUME is blocked while
 * the suspend handler runs (sa_mask), so a resume racing just ahead of the
 * sigsuspend stays pending and is delivered atomically when sigsuspend unblocks
 * it -- no lost wakeup.
 *
 * Gated behind RECOMP_XSUSPEND (default OFF) so it cannot change behaviour
 * unless asked: with it off, cross-thread SuspendThread stays count-only exactly
 * as before. RT signal numbers are a guess for Android/Bionic (SIGRTMIN is
 * shifted past the runtime's reserved ones); if +2/+3 collide, override with
 * RECOMP_XSUSPEND_SIG=<base>. Risks to watch: EINTR on a syscall the target was
 * in, and suspending a thread that holds a lock -- the pool parks its workers
 * when idle, which avoids both, but they are why this is opt-in. */
#ifdef __linux__
static int w32_xsuspend_enabled(void)
{
    static int e = -1;
    if (e < 0) e = getenv("RECOMP_XSUSPEND") ? 1 : 0;
    return e;
}
static int w32_sig_suspend(void)
{
    static int s = -1;
    if (s < 0) {
        const char *o = getenv("RECOMP_XSUSPEND_SIG");
        int base = (o && atoi(o) > 0) ? atoi(o) : (SIGRTMIN + 2);
        s = base;
    }
    return s;
}
static int w32_sig_resume(void) { return w32_sig_suspend() + 1; }

/* The generated guest code's address range (xbox_SetGuestCodeRange). A thread
 * interrupted there holds no host lock, so it may be parked on the spot; one
 * interrupted in host code (a kernel call, libc, the renderer) may hold a lock
 * a running thread needs -- the kernel timer thread blocked behind a parked
 * CRI counter thread and froze the whole game once -- so it parks at its next
 * safe point instead (w32_freeze_point). Unset = park anywhere (old behaviour). */
static uintptr_t s_guest_lo, s_guest_hi;

/* Park until ResumeThread clears the request. RESUME and SUSPEND are blocked
 * around the check, and sigsuspend unblocks RESUME atomically, so a resume
 * that lands between the check and the wait is not lost. */
static void w32_park_for_suspend(w32_object *o)
{
    sigset_t blk, old, wait_mask;
    sigemptyset(&blk);
    sigaddset(&blk, w32_sig_resume());
    sigaddset(&blk, w32_sig_suspend());
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    wait_mask = old;
    sigaddset(&wait_mask, w32_sig_suspend());
    sigdelset(&wait_mask, w32_sig_resume());        /* only RESUME may wake us */
    o->sig_parked = 1;
    while (o->sig_park_req)
        sigsuspend(&wait_mask);                     /* atomic unblock+wait */
    o->sig_parked = 0;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void w32_suspend_handler(int sig, siginfo_t *si, void *uctx)
{
    (void)sig; (void)si;
    int saved = errno;
    w32_object *o = t_self_obj;
    uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)uctx)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uctx;
#endif
    if (o && o->sig_park_req && !o->in_kcall &&
        (!s_guest_hi || !pc || (pc >= s_guest_lo && pc < s_guest_hi)))
        w32_park_for_suspend(o);
    errno = saved;
}
static void w32_resume_handler(int sig) { (void)sig; }  /* just interrupts sigsuspend */

/* Host freeze: stop the whole guest while the app is in the background or its
 * own menu is open (xbox_HostFreeze). Every live CreateThread thread -- the
 * title's threads and the kernel timer/interrupt thread -- parks in a signal
 * handler until thawed, and the guest clocks (GetTickCount*,
 * QueryPerformanceCounter) stand still, so the title sees no time pass.
 *
 * A thread is parked only when the signal lands in this library's own code. In
 * libc or a driver it may hold a lock the UI or binder threads need (malloc,
 * the GPU driver), so it is left to run and signalled again every 20 ms until
 * it is back in our code. A thread blocked in a wait just stays blocked. One
 * that stays busy out there for 2 s is parked where it is (FREEZE_FORCE_TICKS). */
#define W32_LIVE_MAX 256
static pthread_mutex_t s_live_lock = PTHREAD_MUTEX_INITIALIZER;
static w32_object *s_live[W32_LIVE_MAX];
static int s_live_n;
static volatile int s_host_frozen;
static uintptr_t s_code_lo, s_code_hi;    /* this library's executable segment */

static int w32_sig_freeze(void) { return SIGRTMIN + 4; }
static int w32_sig_thaw(void)   { return SIGRTMIN + 5; }

/* List updates run with the freeze signal blocked, so a thread is never parked
 * holding s_live_lock (the freeze and thaw paths take it). */
static void live_update(w32_object *o, int add)
{
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, w32_sig_freeze());
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    pthread_mutex_lock(&s_live_lock);
    if (add) {
        if (s_live_n < W32_LIVE_MAX) s_live[s_live_n++] = o;
    } else {
        for (int i = 0; i < s_live_n; i++)
            if (s_live[i] == o) { s_live[i] = s_live[--s_live_n]; break; }
    }
    pthread_mutex_unlock(&s_live_lock);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void w32_freeze_handler(int sig, siginfo_t *si, void *uctx)
{
    (void)sig; (void)si;
    int saved = errno;
    w32_object *o = t_self_obj;
    uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)uctx)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
#else
    (void)uctx;
#endif
    if (o && __atomic_load_n(&s_host_frozen, __ATOMIC_SEQ_CST)
        && s_code_hi && (pc < s_code_lo || pc >= s_code_hi) && !o->force_park) {
        /* Outside our code: left running (see above). Note where, for
         * freeze_ctl_thread's report if it stays busy out there. */
        o->freeze_pc = pc;
#if defined(__aarch64__)
        {
            const mcontext_t *m = &((ucontext_t *)uctx)->uc_mcontext;
            uintptr_t fp = (uintptr_t)m->regs[29];
            o->freeze_lr = (uintptr_t)m->regs[30];
            for (int i = 0; i < 6; i++) {
                uintptr_t ret = 0;
                if (fp >= o->stk_lo && fp + 16 <= o->stk_hi && !(fp & 7)) {
                    ret = ((const uintptr_t *)fp)[1];
                    fp = ((const uintptr_t *)fp)[0];
                }
                o->freeze_bt[i] = ret;
            }
        }
#endif
    } else if (o && __atomic_load_n(&s_host_frozen, __ATOMIC_SEQ_CST)) {
        sigset_t wait_mask;
        sigfillset(&wait_mask);
        sigdelset(&wait_mask, w32_sig_thaw());      /* only THAW may wake us */
        __atomic_store_n(&o->host_frozen, 1, __ATOMIC_SEQ_CST);
        while (__atomic_load_n(&s_host_frozen, __ATOMIC_SEQ_CST))
            sigsuspend(&wait_mask);
        __atomic_store_n(&o->host_frozen, 0, __ATOMIC_SEQ_CST);
    }
    errno = saved;
}

static int find_code_segment(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    uintptr_t me = (uintptr_t)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X)) continue;
        uintptr_t lo = (uintptr_t)info->dlpi_addr + ph->p_vaddr;
        if (me >= lo && me < lo + ph->p_memsz) {
            s_code_lo = lo;
            s_code_hi = lo + ph->p_memsz;
            return 1;
        }
    }
    return 0;
}

static int64_t thread_cpu_ns(pthread_t t)
{
    clockid_t cid;
    struct timespec ts;
    if (pthread_getcpuclockid(t, &cid) != 0 || clock_gettime(cid, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* A thread still running outside our code this long into a freeze, busy the
 * whole time, is spinning on something the freeze itself stopped (a GPU or
 * display wait that only completes once the app is back): it would keep a
 * core at 100% for as long as the app sits in the background. It is parked
 * wherever it is: whatever lock it holds it holds anyway while it spins, and
 * the thaw resumes it where it stopped, like every other thread. Seen once on
 * the RP6 (1.0.1): the pushbuffer thread spun a core for minutes after Home. */
#define FREEZE_FORCE_TICKS 100          /* x 20 ms */

static void report_forced_park(pid_t tid, uintptr_t pc, uintptr_t lr, const uintptr_t *bt)
{
    char line[1024];
    int k = snprintf(line, sizeof line,
                     "  [FREEZE] tid %d stayed busy outside the game code for 2 s while "
                     "paused; parking it there. pc/lr/frames:", (int)tid);
    uintptr_t at[8] = { pc, lr, bt[0], bt[1], bt[2], bt[3], bt[4], bt[5] };
    for (int i = 0; i < 8 && k < (int)sizeof line - 96; i++) {
        Dl_info di;
        if (!at[i]) continue;
        if (dladdr((void *)at[i], &di) && di.dli_fname) {
            const char *base = strrchr(di.dli_fname, '/');
            k += snprintf(line + k, sizeof line - k, " %s+0x%lx%s%s", base ? base + 1 : di.dli_fname,
                          (unsigned long)(at[i] - (uintptr_t)di.dli_fbase),
                          di.dli_sname ? ":" : "", di.dli_sname ? di.dli_sname : "");
        } else {
            k += snprintf(line + k, sizeof line - k, " 0x%lx", (unsigned long)at[i]);
        }
    }
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

static void *freeze_ctl_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct { pid_t tid; uintptr_t pc, lr, bt[6]; } rep[4];
        int nrep = 0;
        pthread_mutex_lock(&s_live_lock);
        if (__atomic_load_n(&s_host_frozen, __ATOMIC_SEQ_CST))
            for (int i = 0; i < s_live_n; i++) {
                w32_object *o = s_live[i];
                if (o->host_frozen || o->sig_parked)
                    continue;
                /* Busy: more than half of the last tick on a CPU. */
                int64_t cpu = thread_cpu_ns(o->thread);
                int busy = cpu >= 0 && o->cpu_ns_last > 0 && cpu - o->cpu_ns_last > 10000000;
                o->cpu_ns_last = cpu;
                o->busy_ticks = busy ? o->busy_ticks + 1 : 0;
                if (o->busy_ticks == FREEZE_FORCE_TICKS && !o->force_park) {
                    if (nrep < 4) {
                        rep[nrep].tid = o->os_tid;
                        rep[nrep].pc = o->freeze_pc;
                        rep[nrep].lr = o->freeze_lr;
                        memcpy(rep[nrep].bt, (const void *)o->freeze_bt, sizeof rep[nrep].bt);
                        nrep++;
                    }
                    o->force_park = 1;
                }
                pthread_kill(o->thread, w32_sig_freeze());
            }
        pthread_mutex_unlock(&s_live_lock);
        /* Outside the lock: dladdr and stdio take locks the spinner may hold. */
        for (int i = 0; i < nrep; i++)
            report_forced_park(rep[i].tid, rep[i].pc, rep[i].lr, rep[i].bt);
        usleep(20000);
    }
    return NULL;
}
#endif /* __linux__ */

/* Cooperative freeze point, in Sleep and SwitchToThread: a thread looping on
 * those spends nearly all its time in the kernel, so the freeze signal almost
 * always lands in libc and never parks it (the NV2A interrupt reflector spins
 * on SwitchToThread while a vblank is in service). Called from our own code,
 * it holds no libc lock, so it parks here instead. */
static void w32_freeze_point(void)
{
#ifdef __linux__
    w32_object *o = t_self_obj;
    if (!o) return;
    /* A cross-thread suspend that arrived while this thread was in host code
     * (see w32_suspend_handler) takes effect here -- but not from a Sleep
     * inside a kernel call, which may hold a host lock; the outermost kernel
     * call's exit parks it before any guest code runs. */
    if (o->sig_park_req && !o->in_kcall && w32_xsuspend_enabled())
        w32_park_for_suspend(o);
    if (!__atomic_load_n(&s_host_frozen, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&o->host_frozen, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_host_frozen, __ATOMIC_SEQ_CST))
        usleep(10000);                     /* THAW's signal cuts this short */
    __atomic_store_n(&o->host_frozen, 0, __ATOMIC_SEQ_CST);
#endif
}

/* Safe points outside Sleep/SwitchToThread: the kernel brackets every guest
 * kernel call with these. Only the outermost call counts: guest code a kernel
 * call runs (a DPC, an APC) may be inside a host lock the outer call holds. */
void xbox_ThreadKernelEnter(void)
{
#ifdef __linux__
    w32_object *o = t_self_obj;
    if (!o) return;
    if (o->in_kcall == 0) w32_freeze_point();
    o->in_kcall++;
#endif
}

void xbox_ThreadKernelExit(void)
{
#ifdef __linux__
    w32_object *o = t_self_obj;
    if (!o) return;
    if (o->in_kcall > 0 && --o->in_kcall == 0) w32_freeze_point();
#endif
}

void xbox_SetGuestCodeRange(uintptr_t lo, uintptr_t hi)
{
#ifdef __linux__
    if (lo && hi > lo) { s_guest_lo = lo; s_guest_hi = hi; }
#else
    (void)lo; (void)hi;
#endif
}

/* Guest clocks: CLOCK_MONOTONIC minus the time spent frozen. */
static volatile int64_t s_clock_off_ns;
static volatile int64_t s_clock_frozen_at;   /* raw time the freeze began, 0 = running */

static int64_t raw_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t guest_mono_ns(void)
{
    int64_t f = __atomic_load_n(&s_clock_frozen_at, __ATOMIC_ACQUIRE);
    return (f ? f : raw_mono_ns()) - __atomic_load_n(&s_clock_off_ns, __ATOMIC_ACQUIRE);
}

/* For the kernel's hang watchdog: is the guest frozen on purpose, and signal
 * every live guest thread (to have each log where it is). */
int xbox_HostFrozen(void)
{
#ifdef __linux__
    return __atomic_load_n(&s_host_frozen, __ATOMIC_SEQ_CST);
#else
    return 0;
#endif
}

void xbox_HostSignalLiveThreads(int sig)
{
#ifdef __linux__
    pthread_mutex_lock(&s_live_lock);
    for (int i = 0; i < s_live_n; i++)
        pthread_kill(s_live[i]->thread, sig);
    pthread_mutex_unlock(&s_live_lock);
#else
    (void)sig;
#endif
}

/* Freeze (on=1) or thaw (on=0) the guest. Called from the app's UI thread;
 * never blocks on anything a parked thread can hold. */
void xbox_HostFreeze(int on)
{
#ifdef __linux__
    static int init;
    if (!init) {
        init = 1;
        dl_iterate_phdr(find_code_segment, (void *)(uintptr_t)&xbox_HostFreeze);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = w32_freeze_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigfillset(&sa.sa_mask);
        sigaction(w32_sig_freeze(), &sa, NULL);
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = w32_resume_handler;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(w32_sig_thaw(), &sa, NULL);
        pthread_t t;
        pthread_create(&t, NULL, freeze_ctl_thread, NULL);
        pthread_detach(t);
    }
    pthread_mutex_lock(&s_live_lock);
    if (on && !s_host_frozen) {
        __atomic_store_n(&s_clock_frozen_at, raw_mono_ns(), __ATOMIC_RELEASE);
        for (int i = 0; i < s_live_n; i++) {
            s_live[i]->force_park = 0;
            s_live[i]->busy_ticks = 0;
            s_live[i]->cpu_ns_last = 0;
        }
        __atomic_store_n(&s_host_frozen, 1, __ATOMIC_SEQ_CST);
        for (int i = 0; i < s_live_n; i++)
            pthread_kill(s_live[i]->thread, w32_sig_freeze());
    } else if (!on && s_host_frozen) {
        __atomic_store_n(&s_host_frozen, 0, __ATOMIC_SEQ_CST);
        int64_t f = s_clock_frozen_at;
        __atomic_store_n(&s_clock_off_ns, s_clock_off_ns + (raw_mono_ns() - f), __ATOMIC_RELEASE);
        __atomic_store_n(&s_clock_frozen_at, 0, __ATOMIC_RELEASE);
        for (int i = 0; i < s_live_n; i++) {
            s_live[i]->force_park = 0;
            if (s_live[i]->host_frozen)
                pthread_kill(s_live[i]->thread, w32_sig_thaw());
        }
    }
    pthread_mutex_unlock(&s_live_lock);
#else
    (void)on;
#endif
}

#ifdef __linux__

static void w32_ensure_suspend_signals(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = w32_suspend_handler;
        /* SA_RESTART: a thread signalled inside a host syscall (a file read)
         * returns from the handler without parking and must not see EINTR. */
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigfillset(&sa.sa_mask);                    /* block everything in-handler */
        sigaction(w32_sig_suspend(), &sa, NULL);
    }
    {
        struct sigaction sr;
        memset(&sr, 0, sizeof(sr));
        sr.sa_handler = w32_resume_handler;
        sigemptyset(&sr.sa_mask);
        sigaction(w32_sig_resume(), &sr, NULL);
    }
}
#endif /* __linux__ */

static void *thread_trampoline(void *arg)
{
    w32_object *o = (w32_object *)arg;
    t_self_obj = o;
    t_tid      = o->tid;
#ifdef __linux__
    o->os_tid = gettid();
    {   /* stack bounds, for the freeze handler's frame-pointer walk */
        pthread_attr_t a;
        void *lo;
        size_t sz;
        if (pthread_getattr_np(pthread_self(), &a) == 0) {
            if (pthread_attr_getstack(&a, &lo, &sz) == 0) {
                o->stk_lo = (uintptr_t)lo;
                o->stk_hi = (uintptr_t)lo + sz;
            }
            pthread_attr_destroy(&a);
        }
    }
    live_update(o, 1);
#endif

    /* Diagnostic (RECOMP_THREAD_LOG): confirm every pthread actually starts and
     * runs its start routine on Android. Guest worker threads spawn through here
     * (start == bridge_thread_main); if this never fires from a new OS tid, the
     * pool workers are not executing. Cheap: one line per thread creation. */
    if (getenv("RECOMP_THREAD_LOG")) {
        fprintf(stderr, "  [W32THREAD] trampoline ENTER ostid=%d w32tid=%u start=%p\n",
                gettid(), (unsigned)o->tid, (void *)(uintptr_t)o->start);
        fflush(stderr);
    }

    /* CREATE_SUSPENDED gate */
    pthread_mutex_lock(&o->lock);
    while (o->suspend_count > 0)
        pthread_cond_wait(&o->gate, &o->lock);
    pthread_mutex_unlock(&o->lock);

    DWORD rc = o->start ? o->start(o->start_param) : 0;

#ifdef __linux__
    live_update(o, 0);
#endif
    pthread_mutex_lock(&o->lock);
    o->exit_code = rc;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    obj_release(o);   /* drop the trampoline's reference */
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId)
{
    (void)sa;
    w32_object *o = obj_alloc(K_THREAD);
    o->start         = start;
    o->start_param   = param;
    o->tid           = (DWORD)InterlockedIncrement(&s_next_tid);
    o->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    o->refcount      = 2;   /* one for caller, one for the trampoline */

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize)
        pthread_attr_setstacksize(&attr, stackSize < 65536 ? 65536 : stackSize);

    if (pthread_create(&o->thread, &attr, thread_trampoline, o) != 0) {
        pthread_attr_destroy(&attr);
        o->refcount = 1;
        obj_release(o);
        SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    o->thread_joinable = 1;

    if (threadId) *threadId = o->tid;
    return (HANDLE)o;
}

VOID ExitThread(DWORD exitCode)
{
    w32_object *o = t_self_obj;
    if (o) {
#ifdef __linux__
        live_update(o, 0);
#endif
        pthread_mutex_lock(&o->lock);
        o->exit_code = exitCode;
        o->exited    = 1;
        o->signaled  = 1;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        obj_release(o);
    }
    pthread_exit(NULL);
}

BOOL GetExitCodeThread(HANDLE h, LPDWORD exitCode)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_THREAD || !exitCode) return FALSE;
    pthread_mutex_lock(&o->lock);
    *exitCode = o->exited ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

DWORD ResumeThread(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    int woke = 0;
    if (o->suspend_count > 0 && --o->suspend_count == 0) {
        pthread_cond_broadcast(&o->gate);   /* wakes the self-suspend / start-gate */
        o->sig_park_req = 0;                 /* count hit 0: release a cross-thread park */
        woke = 1;
    }
    pthread_mutex_unlock(&o->lock);
#ifdef __linux__
    /* Wake a cross-thread parked target: clear the request (above) then interrupt
     * its sigsuspend. RESUME was blocked while the suspend handler ran, so if it
     * races just ahead of the sigsuspend it stays pending and is delivered when
     * sigsuspend unblocks it -- no lost wakeup. */
    if (woke && w32_xsuspend_enabled() && o->sig_parked)
        pthread_kill(o->thread, w32_sig_resume());
#endif
    return prev;
}

DWORD SuspendThread(HANDLE h)
{
    /* Real SELF-suspension. A thread suspending ITSELF blocks on the same gate
     * the CREATE_SUSPENDED start-gate and ResumeThread use, until the count
     * returns to 0 (ResumeThread broadcasts that gate). The Xbox thread pool
     * relies on exactly this: each worker loops "process work, then
     * NtSuspendThread(self)" to sleep until the submitter calls NtResumeThread.
     * Leaving suspend a no-op meant the workers never blocked, so the pool's
     * suspend/resume handshake broke and the intro's wait-for-completion
     * (sub_000F97B0 spinning on flag [0x4240f8]) never cleared -> the freeze.
     * The while-loop re-checks the count, so a resume that races just ahead of
     * the suspend is not lost. Suspending ANOTHER thread mid-run is still
     * unsupported on POSIX (it needs a signal), but this pool -- and the common
     * case -- only ever suspends self. */
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : (w32_object *)h;
    if (!o || o->kind != K_THREAD) {
        if (getenv("RECOMP_THREAD_LOG")) {
            fprintf(stderr, "  [W32THREAD] SuspendThread REJECT h=%p o=%p kind=%d "
                    "(not a thread) ostid=%d\n", (void *)h, (void *)o,
                    o ? o->kind : -1, gettid());
            fflush(stderr);
        }
        return (DWORD)-1;
    }
    /* Is this a self-suspend? The pool self-suspends its workers; only that case
     * can block on POSIX. Match either the resolved object's thread or the
     * caller's own self-object, so a differently-tagged handle for the current
     * thread still counts as self. */
    int is_self = pthread_equal(o->thread, pthread_self()) || (o == t_self_obj);
    if (getenv("RECOMP_THREAD_LOG")) {
        static int n;
        if (n++ < 40) {
            fprintf(stderr, "  [W32THREAD] SuspendThread h=%p o=%p self=%d "
                    "(o->thread==caller=%d o==t_self=%d) count=%d ostid=%d\n",
                    (void *)h, (void *)o, is_self,
                    (int)pthread_equal(o->thread, pthread_self()),
                    (int)(o == t_self_obj), o->suspend_count, gettid());
            fflush(stderr);
        }
    }
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    o->suspend_count++;
    o->sig_park_req = (o->suspend_count > 0);   /* mirror the count for the handler */
    if (is_self) {
        while (o->suspend_count > 0)
            pthread_cond_wait(&o->gate, &o->lock);
        pthread_mutex_unlock(&o->lock);
        return prev;
    }
    pthread_mutex_unlock(&o->lock);
#ifdef __linux__
    /* Cross-thread suspend (opt-in, RECOMP_XSUSPEND): signal the target to park
     * in the handler. The pool's main thread uses this to park its workers for
     * reuse; without it they run to completion and die. */
    if (w32_xsuspend_enabled()) {
        w32_ensure_suspend_signals();
        pthread_kill(o->thread, w32_sig_suspend());
        /* Return once the target can no longer run guest code: parked, or
         * inside a kernel call (it parks on the way out, before any guest
         * code). A target interrupted in other host code (libc, an MMIO trap)
         * parks the next time a signal finds it in guest code, so re-send
         * now and then. Bounded, so a lost/ignored signal (e.g. a target with
         * no t_self_obj yet) degrades to the old no-op instead of hanging the
         * caller. */
        int i;
        for (i = 0; i < 200000 && !o->sig_parked && !o->in_kcall; i++) {
            if ((i & 1023) == 1023 && o->sig_park_req)
                pthread_kill(o->thread, w32_sig_suspend());
            sched_yield();
        }
        if (i == 200000) {
            static int n;
            if (n++ < 5) {
                fprintf(stderr, "  [W32THREAD] SuspendThread: target tid %u did not park; "
                        "continuing\n", (unsigned)o->tid);
                fflush(stderr);
            }
        }
    }
#endif
    return prev;
}

BOOL TerminateThread(HANDLE h, DWORD exitCode)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_THREAD) return FALSE;
    pthread_cancel(o->thread);
    pthread_mutex_lock(&o->lock);
    o->exit_code = exitCode;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : (w32_object *)h;
    if (o && o->kind == K_THREAD) o->priority = priority;
    return TRUE;   /* real RT priorities need privileges; tracked only */
}

int GetThreadPriority(HANDLE h)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : (w32_object *)h;
    return (o && o->kind == K_THREAD) ? o->priority : THREAD_PRIORITY_NORMAL;
}

VOID SwitchToThread(void) { w32_freeze_point(); sched_yield(); }

DWORD QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data)
{
    w32_object *o = (thread == PSEUDO_CURRENT_THREAD) ? t_self_obj : (w32_object *)thread;
    if (!o || o->kind != K_THREAD) return 0;
    pthread_mutex_lock(&o->lock);
    DWORD ok = 0;
    if (o->apc_count < W32_MAX_APC) {
        o->apc_func[o->apc_count] = func;
        o->apc_data[o->apc_count] = data;
        o->apc_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

/* ===================================================================== */
/* Sleep                                                                 */
/* ===================================================================== */

VOID Sleep(DWORD ms)
{
    w32_freeze_point();
    if (ms == 0) { sched_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    Sleep(ms);
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return 0;
}

/* ===================================================================== */
/* Timer queues (one helper thread per timer)                            */
/* ===================================================================== */

static void *timer_thread(void *arg)
{
    w32_object *o = (w32_object *)arg;
    int once = (o->timer_period == 0);

    /* initial due time */
    if (o->timer_due) Sleep(o->timer_due);
    if (!o->timer_cancel && o->timer_cb)
        o->timer_cb(o->timer_param, TRUE);

    while (!once && !o->timer_cancel) {
        Sleep(o->timer_period);
        if (o->timer_cancel) break;
        if (o->timer_cb) o->timer_cb(o->timer_param, TRUE);
    }
    obj_release(o);
    return NULL;
}

HANDLE CreateTimerQueue(void)
{
    /* A timer queue is just a grouping token here. */
    return (HANDLE)obj_alloc(K_TIMER);
}

BOOL DeleteTimerQueue(HANDLE timerQueue)
{
    return CloseHandle(timerQueue);
}

BOOL CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                           WAITORTIMERCALLBACK callback, PVOID param,
                           DWORD dueTime, DWORD period, ULONG flags)
{
    (void)timerQueue;
    if (flags & WT_EXECUTEONLYONCE) period = 0;
    w32_object *o = obj_alloc(K_TIMER);
    o->timer_cb     = callback;
    o->timer_param  = param;
    o->timer_due    = dueTime;
    o->timer_period = period;
    o->refcount     = 2;   /* caller + timer thread */

    if (pthread_create(&o->thread, NULL, timer_thread, o) != 0) {
        o->refcount = 1;
        obj_release(o);
        return FALSE;
    }
    pthread_detach(o->thread);
    if (newTimer) *newTimer = (HANDLE)o;
    return TRUE;
}

BOOL ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_due    = dueTime;
    o->timer_period = period;
    return TRUE;
}

BOOL DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_cancel = 1;
    if (completionEvent) SetEvent(completionEvent);
    obj_release(o);
    return TRUE;
}

struct w32_tp_args { PTP_SIMPLE_CALLBACK cb; PVOID ctx; };

static void *w32_tp_trampoline(void *arg)
{
    struct w32_tp_args *a = (struct w32_tp_args *)arg;
    a->cb(NULL, a->ctx);
    free(a);
    return NULL;
}

BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                 PVOID context, PVOID env)
{
    (void)env;
    /* Run on a throwaway detached thread. */
    struct w32_tp_args *a = (struct w32_tp_args *)malloc(sizeof(*a));
    a->cb = callback; a->ctx = context;
    pthread_t th;
    if (pthread_create(&th, NULL, w32_tp_trampoline, a) != 0) { free(a); return FALSE; }
    pthread_detach(th);
    return TRUE;
}

/* ===================================================================== */
/* Waitable timers                                                       */
/* ===================================================================== */

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, LPCWSTR name)
{
    (void)sa;
    (void)name;
    w32_object *o = obj_alloc(K_WAITABLE_TIMER);
    o->waitable_manual_reset = manualReset;
    return (HANDLE)o;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *dueTime, LONG period,
                      PTIMERAPCROUTINE completion, PVOID arg, BOOL resume)
{
    w32_object *o = (w32_object *)h;
    (void)completion; (void)arg; (void)resume;
    if (!o || o->kind != K_WAITABLE_TIMER || !dueTime) return FALSE;
    pthread_mutex_lock(&o->lock);
    /* Win32 100ns units: negative is relative to now, positive is an absolute
     * FILETIME. ponytail: period is ignored -- one-shot only, revisit if a
     * title actually arms a repeating timer. */
    if (dueTime->QuadPart <= 0) {
        clock_gettime(CLOCK_REALTIME, &o->waitable_due_time);
        LONGLONG ns = -dueTime->QuadPart * 100LL;
        o->waitable_due_time.tv_sec  += (time_t)(ns / 1000000000LL);
        o->waitable_due_time.tv_nsec += (long)(ns % 1000000000LL);
        if (o->waitable_due_time.tv_nsec >= 1000000000L) {
            o->waitable_due_time.tv_sec++;
            o->waitable_due_time.tv_nsec -= 1000000000L;
        }
    } else {
        /* FILETIME epoch is 1601-01-01; Unix is 1970-01-01. */
        LONGLONG unix100ns = dueTime->QuadPart - 116444736000000000LL;
        o->waitable_due_time.tv_sec  = (time_t)(unix100ns / 10000000LL);
        o->waitable_due_time.tv_nsec = (long)((unix100ns % 10000000LL) * 100LL);
    }
    (void)period;
    o->waitable_armed = 1;
    o->waitable_triggered = 0;
    pthread_mutex_unlock(&o->lock);
    pthread_cond_broadcast(&o->cond);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_WAITABLE_TIMER) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->waitable_triggered = 0;
    o->waitable_armed = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Heap (thin wrapper over malloc; the single process heap)              */
/* ===================================================================== */

static w32_object s_process_heap = { .kind = K_HEAP };

HANDLE GetProcessHeap(void)                       { return (HANDLE)&s_process_heap; }
HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m)    { (void)o;(void)i;(void)m; return (HANDLE)&s_process_heap; }
BOOL   HeapDestroy(HANDLE h)                      { (void)h; return TRUE; }

LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    (void)heap;
    return (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes ? bytes : 1)
                                      : malloc(bytes ? bytes : 1);
}
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes)
{
    (void)heap; (void)flags;
    return realloc(mem, bytes ? bytes : 1);
}
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem)
{
    (void)heap; (void)flags;
    free(mem);
    return TRUE;
}
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem)
{
    (void)heap; (void)flags; (void)mem;
    return 0;   /* glibc malloc_usable_size could be used; not needed yet */
}

/* ===================================================================== */
/* Virtual memory                                                        */
/* ===================================================================== */

static int prot_from_page(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return PROT_READ | PROT_WRITE;
    }
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    int prot  = prot_from_page(protect);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    /* MEM_COMMIT on a region already reserved by a prior VirtualAlloc:
     * just adjust protection. */
    if ((allocationType & MEM_COMMIT) && !(allocationType & MEM_RESERVE) && address) {
        if (mprotect(address, size, prot) == 0)
            return address;
        /* fall through to a fresh mapping */
    }

#if defined(MAP_FIXED_NOREPLACE)
    if (address) flags |= MAP_FIXED_NOREPLACE;
#elif defined(__APPLE__)
    /* TODO: mach_vm_map with VM_FLAGS_FIXED (which does fail rather than replace),
     * or a mach_vm_region probe before an MAP_FIXED call. */
#endif
    void *p = mmap(address, size, prot ? prot : PROT_READ | PROT_WRITE,
                   flags, -1, 0);
    if (p == MAP_FAILED) { SetLastError(8); return NULL; }
#if !defined(MAP_FIXED_NOREPLACE)
    /* TODO: Without MAP_FIXED_NOREPLACE (macOS or older kernels) plain
     * MAP_FIXED would silently unmap whatever already lives there. Getting a
     * different address means the range was taken: fail as Linux does. */
#endif
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    if (freeType & MEM_RELEASE) {
        /* Win32 MEM_RELEASE passes size 0; we can't know the length, so this
         * path is only safe when callers pass the real size. */
        if (size == 0) return TRUE;
        return munmap(address, size) == 0;
    }
    if (freeType & MEM_DECOMMIT)
        return mprotect(address, size, PROT_NONE) == 0;
    return TRUE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    if (oldProtect) *oldProtect = PAGE_READWRITE;
    return mprotect(address, size, prot_from_page(newProtect)) == 0;
}

/* ===================================================================== */
/* Time                                                                  */
/* ===================================================================== */

/* 100-ns intervals between 1601-01-01 and 1970-01-01 */
#define FILETIME_EPOCH_DIFF 116444736000000000ULL

VOID GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ULONGLONG t = FILETIME_EPOCH_DIFF
                + (ULONGLONG)ts.tv_sec * 10000000ULL
                + (ULONGLONG)ts.tv_nsec / 100ULL;
    ft->dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void fill_systemtime(LPSYSTEMTIME st, const struct tm *tm, long nsec)
{
    st->wYear         = (WORD)(tm->tm_year + 1900);
    st->wMonth        = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek    = (WORD)tm->tm_wday;
    st->wDay          = (WORD)tm->tm_mday;
    st->wHour         = (WORD)tm->tm_hour;
    st->wMinute       = (WORD)tm->tm_min;
    st->wSecond       = (WORD)tm->tm_sec;
    st->wMilliseconds = (WORD)(nsec / 1000000L);
}

VOID GetSystemTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

VOID GetLocalTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

ULONGLONG GetTickCount64(void)
{
    return (ULONGLONG)guest_mono_ns() / 1000000ULL;   /* stands still while frozen */
}

DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(PLARGE_INTEGER count)
{
    count->QuadPart = (LONGLONG)guest_mono_ns();      /* stands still while frozen */
    return TRUE;
}

BOOL QueryPerformanceFrequency(PLARGE_INTEGER freq)
{
    freq->QuadPart = 1000000000LL;   /* QPC is in nanoseconds */
    return TRUE;
}

/* ===================================================================== */
/* Misc                                                                  */
/* ===================================================================== */

VOID OutputDebugStringA(LPCSTR str)
{
    if (str) fputs(str, stderr);
}

VOID ExitProcess(UINT exitCode) { exit((int)exitCode); }

BOOL IsDebuggerPresent(void)
{
#if defined(__APPLE__)
    /* Darwin: KERN_PROC_PID reports P_TRACED when a debugger is attached. */
    struct kinfo_proc info;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    size_t size = sizeof(info);
    memset(&info, 0, size);
    if (sysctl(mib, sizeof(mib), &info, &size, NULL, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    /* Linux: a non-zero TracerPid in /proc/self/status means ptrace is attached. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            traced = strtol(line + 10, NULL, 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
#endif
}

VOID DebugBreak(void) {
    /* Not __debugbreak(): that is an MSVC intrinsic, and this file is the
     * half that MSVC never compiles. SIGTRAP is the POSIX equivalent --
     * continuable under a debugger, fatal without one, as on Windows. */
    raise(SIGTRAP);
}

VOID SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
#if defined(__APPLE__)
    memset_s(ptr, cnt, 0, cnt);
#else
    explicit_bzero(ptr, cnt);
#endif
}

unsigned int _clearfp(void)
{
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

/* ===================================================================== */
/* Win32 file API on POSIX (open/read/write/fstat-backed)                */
/* ===================================================================== */

#include <fcntl.h>
#include <sys/stat.h>

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    (void)share; (void)sa; (void)flags; (void)templ;
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }

    int rw = O_RDONLY;
    int wantW = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    int wantR = (access & (GENERIC_READ  | GENERIC_ALL)) != 0;
    if (wantW && wantR) rw = O_RDWR;
    else if (wantW)     rw = O_WRONLY;

    int extra = 0;
    switch (disp) {
    case CREATE_NEW:        extra = O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     extra = O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     extra = 0;                 break;
    case OPEN_ALWAYS:       extra = O_CREAT;           break;
    case TRUNCATE_EXISTING: extra = O_TRUNC;           break;
    default:                extra = 0;                 break;
    }
    if ((extra & (O_CREAT | O_TRUNC)) && rw == O_RDONLY) rw = O_RDWR;

    /* Normalise embedded Windows-style backslashes before open(). */
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s", name);
    xbox_path_normalize(norm);
    int fd = open(norm, rw | extra, 0644);
    if (fd < 0) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return w32_open_handle(fd, name);
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    /* Basic UTF-16 -> UTF-8 (ASCII path of WideCharToMultiByte). */
    char buf[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL);
    if (len <= 0) buf[0] = '\0';
    return CreateFileA(buf, access, share, sa, disp, flags, templ);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nread) *nread = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    /* Win32 ReadFile on a file handle fills the whole buffer unless it hits EOF.
     * POSIX read() may return short (esp. large reads on Android FUSE storage),
     * so loop. A single read() truncated large save/level loads. */
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char *)buf + total, (size_t)len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (total == 0) { if (nread) *nread = 0; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
            break;
        }
        if (n == 0) break;              /* EOF */
        total += (size_t)n;
    }
    if (nread)  *nread = (DWORD)total;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nwritten) *nwritten = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    /* Loop to commit the whole buffer -- POSIX write() may be short on FUSE. */
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, (const char *)buf + total, (size_t)len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (total == 0) { if (nwritten) *nwritten = 0; SetLastError(ERROR_GEN_FAILURE); return FALSE; }
            break;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    if (nwritten) *nwritten = (DWORD)total;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return INVALID_FILE_SIZE;
    struct stat st;
    if (fstat(fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(((uint64_t)st.st_size >> 32) & 0xFFFFFFFFu);
    return (DWORD)(st.st_size & 0xFFFFFFFFu);
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    int fd = w32_handle_fd(h);
    if (fd < 0 || !size) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    struct stat st;
    if (fstat(fd, &st) != 0) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return FALSE;
    return fsync(fd) == 0;
}

/* ===================================================================== */
/* Keyboard + window helpers -- stubs. Real keyboard polling will come   */
/* via SDL_GetKeyboardState when main.c gets its SDL2 port.              */
/* ===================================================================== */

SHORT GetAsyncKeyState(int vKey)          { (void)vKey; return 0; }
HWND  FindWindowA(LPCSTR c, LPCSTR w)     { (void)c; (void)w; return NULL; }
HWND  GetActiveWindow(void)               { return NULL; }
BOOL  SetWindowTextA(HWND h, LPCSTR t)    { (void)h; (void)t; return TRUE; }
int   GetWindowTextA(HWND h, LPSTR t, int n) { (void)h; (void)t; (void)n; return 0; }
BOOL  EnumWindows(WNDENUMPROC p, LPARAM l) { (void)p; (void)l; return FALSE; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "MessageBox",
                                   text    ? text    : "");
    return 1;   /* IDOK */
}

/* Message-loop stubs: no Win32 messages on POSIX (SDL events drive the
 * d3d8_gl backend; this layer is just for the game's Win32 message pump). */
BOOL    PeekMessageA(LPMSG m, HWND w, UINT a, UINT b, UINT f)
{ (void)m; (void)w; (void)a; (void)b; (void)f; return FALSE; }
BOOL    TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageA(const MSG *m) { (void)m; return 0; }

/* XInput stub: real gamepad is wired through input_compat (SDL2). */
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state)
{ (void)idx; if (state) memset(state, 0, sizeof(*state)); return ERROR_DEVICE_NOT_CONNECTED; }

BOOL TerminateProcess(HANDLE process, UINT exitCode)
{
    (void)process;
    exit((int)exitCode);
}

VOID OutputDebugStringW(LPCWSTR str)
{
    if (!str) return;
    for (const WCHAR *p = str; *p; p++)
        fputc((*p < 128) ? (int)*p : '?', stderr);
}

/*
 * Minimal MultiByteToWideChar / WideCharToMultiByte. Handles UTF-8 and a
 * latin-1 interpretation of CP_ACP -- enough for path/name strings.
 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                        LPWSTR wide, int wideCount)
{
    (void)flags;
    if (!mb) return 0;
    int srcLen = (mbCount < 0) ? (int)strlen(mb) + 1 : mbCount;
    int out = 0;

    for (int i = 0; i < srcLen; ) {
        unsigned int cpval;
        unsigned char c = (unsigned char)mb[i];

        if (cp == CP_UTF8 && c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < srcLen) {
                cpval = ((c & 0x1F) << 6) | (mb[i+1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < srcLen) {
                cpval = ((c & 0x0F) << 12) | ((mb[i+1] & 0x3F) << 6) |
                        (mb[i+2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < srcLen) {
                cpval = ((c & 0x07) << 18) | ((mb[i+1] & 0x3F) << 12) |
                        ((mb[i+2] & 0x3F) << 6) | (mb[i+3] & 0x3F); i += 4;
            } else { cpval = c; i += 1; }
        } else {
            cpval = c; i += 1;   /* ASCII / latin-1 */
        }

        if (cpval > 0xFFFF) cpval = '?';   /* no surrogate pairs */
        if (wideCount > 0) {
            if (out >= wideCount) return 0;
            wide[out] = (WCHAR)cpval;
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                        LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef)
{
    (void)flags; (void)defChar; (void)usedDef;
    if (!wide) return 0;
    int srcLen = wideCount;
    if (srcLen < 0) { srcLen = 0; while (wide[srcLen]) srcLen++; srcLen++; }
    int out = 0;

    for (int i = 0; i < srcLen; i++) {
        unsigned int cpval = wide[i];
        char buf[4]; int n;
        if (cp == CP_UTF8 && cpval >= 0x80) {
            if (cpval < 0x800) {
                buf[0] = (char)(0xC0 | (cpval >> 6));
                buf[1] = (char)(0x80 | (cpval & 0x3F)); n = 2;
            } else {
                buf[0] = (char)(0xE0 | (cpval >> 12));
                buf[1] = (char)(0x80 | ((cpval >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (cpval & 0x3F)); n = 3;
            }
        } else {
            buf[0] = (char)(cpval > 0xFF ? '?' : cpval); n = 1;
        }
        if (mbCount > 0) {
            if (out + n > mbCount) return 0;
            for (int k = 0; k < n; k++) mb[out + k] = buf[k];
        }
        out += n;
    }
    return out;
}

/* ===================================================================== */
/* File mapping (memfd-backed) -- true aliased mirror views              */
/* ===================================================================== */

/* Registry of active views: UnmapViewOfFile takes no length, so we must
 * recover the mapping length here for munmap. */
typedef struct { void *addr; size_t len; } w32_view;
static w32_view        s_views[512];
static pthread_mutex_t s_views_lock = PTHREAD_MUTEX_INITIALIZER;

static void view_register(void *addr, size_t len)
{
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (!s_views[i].addr) { s_views[i].addr = addr; s_views[i].len = len; break; }
    pthread_mutex_unlock(&s_views_lock);
}

static size_t view_take(const void *addr)
{
    size_t len = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (s_views[i].addr == addr) { len = s_views[i].len; s_views[i].addr = NULL; break; }
    pthread_mutex_unlock(&s_views_lock);
    return len;
}

/* An unnamed file descriptor that ftruncate and mmap both accept. Linux has
 * memfd_create for this; elsewhere an immediately-unlinked temp file does. */
static int anon_map_fd(const char *name)
{
#if defined(__APPLE__)
    static volatile LONG map_counter = 0;
    char shm_name[32];
    LONG seq = InterlockedIncrement(&map_counter);
    const char *base = name ? name : "xbox_map";
    snprintf(shm_name, sizeof(shm_name), "/%s_%ld", base, seq);
    int fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(shm_name);
    return fd;
#else
    return memfd_create(name ? name : "xbox_map", 0);
#endif
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name)
{
    (void)file; (void)sa; (void)protect;
    SIZE_T size = ((SIZE_T)maxSizeHigh << 32) | maxSizeLow;
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    int fd = anon_map_fd(name);
    if (fd < 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    w32_object *o = obj_alloc(K_FILEMAP);
    o->fd       = fd;
    o->map_size = size;
    return (HANDLE)o;
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name)
{
    (void)name;
    return CreateFileMappingA(file, sa, protect, maxSizeHigh, maxSizeLow, NULL);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow,
                       SIZE_T count, LPVOID baseAddr)
{
    w32_object *o = (w32_object *)mapping;
    if (!o || o->kind != K_FILEMAP) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }

    off_t  off = ((off_t)offHigh << 32) | offLow;
    SIZE_T len = count ? count : (o->map_size - (SIZE_T)off);
    int prot   = PROT_READ | ((access != FILE_MAP_READ) ? PROT_WRITE : 0);
    int flags  = MAP_SHARED;
    if (baseAddr) {
        /* Win32 MapViewOfFileEx at a fixed base FAILS if the range is occupied;
         * it never stomps. Plain MAP_FIXED does stomp, which silently unmapped
         * host memory (the base-view fallback loop in xbox_memory_layout.c then
         * "succeeds" at the first probe and corrupts the process). Use
         * MAP_FIXED_NOREPLACE so a collision returns NULL and the caller's
         * next-base / OS-choose fallback runs, matching Win32. (Android port.) */
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#else
        flags |= MAP_FIXED;
#endif
    }

    void *p = mmap(baseAddr, len, prot, flags, o->fd, off);
    if (p == MAP_FAILED) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    view_register(p, len);
    return p;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    size_t len = view_take(baseAddr);
    if (len == 0) return FALSE;
    return munmap((void *)baseAddr, len) == 0;
}

/* ===================================================================== */
/* VirtualQuery                                                           */
/* ===================================================================== */

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length)
{
    if (!buffer || length < sizeof(*buffer)) return 0;
    memset(buffer, 0, sizeof(*buffer));
    buffer->BaseAddress    = (PVOID)address;
    buffer->AllocationBase = NULL;       /* != address -> freed via _aligned_free */
    buffer->RegionSize     = 0x1000;
    buffer->State          = MEM_COMMIT;
    buffer->Protect        = PAGE_READWRITE;
    buffer->Type           = 0x20000;    /* MEM_PRIVATE */
    return sizeof(*buffer);
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX b)
{
    if (!b) return FALSE;
#if defined(__APPLE__)
    /* Darwin has no sysinfo(2): physical memory comes from sysctl hw.memsize,
     * swap from vm.swapusage, the free page count from the Mach VM statistics. */
    uint64_t memsize = 0;
    size_t   len     = sizeof(memsize);
    int oid_memsize[] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(oid_memsize, 2, &memsize, &len, NULL, 0) != 0) return FALSE;

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) page = 4096;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    ULONGLONG avail = 0;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS)
        avail = ((ULONGLONG)vm.free_count + vm.inactive_count) * (ULONGLONG)page;

    struct xsw_usage swap;
    len = sizeof(swap);
    int oid_swapusage[] = { CTL_VM, VM_SWAPUSAGE };
    if (sysctl(oid_swapusage, 2, &swap, &len, NULL, 0) != 0)
        memset(&swap, 0, sizeof(swap));

    b->ullTotalPhys     = (ULONGLONG)memsize;
    b->ullAvailPhys     = avail;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)swap.xsu_total;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)swap.xsu_avail;
#else
    struct sysinfo si;
    if (sysinfo(&si) != 0) return FALSE;

    ULONGLONG unit = si.mem_unit ? si.mem_unit : 1;
    b->ullTotalPhys     = (ULONGLONG)si.totalram  * unit;
    b->ullAvailPhys     = (ULONGLONG)si.freeram   * unit;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)si.totalswap * unit;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)si.freeswap  * unit;
#endif
    b->ullTotalVirtual  = b->ullTotalPhys;
    b->ullAvailVirtual  = b->ullAvailPhys;
    b->ullAvailExtendedVirtual = 0;
    b->dwMemoryLoad = b->ullTotalPhys
        ? (DWORD)(100 - (b->ullAvailPhys * 100 / b->ullTotalPhys)) : 0;
    return TRUE;
}

/* ===================================================================== */
/* Aligned allocation                                                     */
/* ===================================================================== */

void *_aligned_malloc(SIZE_T size, SIZE_T alignment)
{
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* round alignment up to a power of two */
    SIZE_T a = sizeof(void *);
    while (a < alignment) a <<= 1;
    void *p = NULL;
    if (posix_memalign(&p, a, size ? size : 1) != 0) return NULL;
    return p;
}

void _aligned_free(void *ptr) { free(ptr); }

/* ===================================================================== */
/* Case-insensitive string compare                                        */
/* ===================================================================== */

int _stricmp(const char *a, const char *b)            { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, SIZE_T n) { return strncasecmp(a, b, n); }

/* ===================================================================== */
/* Wide-string helpers (16-bit Xbox WCHAR)                                */
/* ===================================================================== */

SIZE_T xbox_wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0)    return 0;
    }
    return 0;
}

WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src)
{
    SIZE_T d = xbox_wcslen(dst), i = 0;
    while (src[i]) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
    return dst;
}

WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src)
{
    SIZE_T i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

/* ===================================================================== */
/* Time conversion                                                        */
/* ===================================================================== */

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon  = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min  = st->wMinute;
    tm.tm_sec  = st->wSecond;
    time_t t = timegm(&tm);
    ULONGLONG ticks = FILETIME_EPOCH_DIFF
                    + (ULONGLONG)t * 10000000ULL
                    + (ULONGLONG)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG ticks = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < FILETIME_EPOCH_DIFF) { memset(st, 0, sizeof(*st)); return FALSE; }
    ULONGLONG since = ticks - FILETIME_EPOCH_DIFF;
    time_t t = (time_t)(since / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fill_systemtime(st, &tm, (long)((since % 10000000ULL) * 100ULL));
    return TRUE;
}

/* ===================================================================== */
/* Exception handling (compile-shim -- SEH not yet emulated on Linux)     */
/* ===================================================================== */

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    (void)TargetFrame; (void)TargetIp; (void)ExceptionRecord; (void)ReturnValue;
    /* TODO: Windows SEH unwinding is not yet emulated on Linux. */
}

VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    (void)flags; (void)nargs; (void)args;
    fprintf(stderr, "[win32_compat] RaiseException(0x%08X): SEH not emulated\n", code);
    /* TODO: on Windows this does not return; SEH dispatch unimplemented. */
}

PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
{ (void)First; (void)Handler; return NULL; }   /* TODO: wire to sigaction */
ULONG RemoveVectoredExceptionHandler(PVOID h) { (void)h; return 1; }

#endif /* !_WIN32 */
