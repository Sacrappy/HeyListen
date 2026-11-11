/* TICK-062 B1: the early return of SignalConditionVariable with no registered waiter (host/nn_os_sync.c, HOS_CV_SIGNAL_FAST) loses
   no wake-up. A model of the protocol with the same steps and memory orders: a waiter reads gen and registers (cv_waiting++,
   seq_cst) under cv->lk, then releases the guest mutex, then sleeps while gen is unchanged; a signal either returns at once on
   cv_waiting == 0 (seq_cst load) or bumps gen under cv->lk and wakes. The guest discipline under test is the usual one: the
   signaller takes the mutex around the state change and the signal, the waiter tests the predicate with the mutex held.
   Every consumer must finish (a lost wake-up hangs: the watchdog fails the test), in both modes.
   build: clang -O2 cv_signal_fast_test.c -o cv_signal_fast_test.exe */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
typedef struct { SRWLOCK lk; CONDITION_VARIABLE cv; uint64_t gen; int waiting; } CV;
static CV cv = { SRWLOCK_INIT, CONDITION_VARIABLE_INIT, 0, 0 };
static CRITICAL_SECTION gm;   /* the guest mutex */
static int fast, items, consumed, produced_total, nprod = 2, ncons = 4;
static volatile LONG done;

static void wait_cv(void) {   /* called with gm held; returns with gm held */
    AcquireSRWLockExclusive(&cv.lk);
    const uint64_t gen = cv.gen;
    __atomic_add_fetch(&cv.waiting, 1, __ATOMIC_SEQ_CST);
    LeaveCriticalSection(&gm);
    while (cv.gen == gen) SleepConditionVariableSRW(&cv.cv, &cv.lk, INFINITE, 0);
    __atomic_sub_fetch(&cv.waiting, 1, __ATOMIC_SEQ_CST);
    ReleaseSRWLockExclusive(&cv.lk);
    EnterCriticalSection(&gm);
}
static void signal_cv(void) {
    if (fast && !__atomic_load_n(&cv.waiting, __ATOMIC_SEQ_CST)) return;
    AcquireSRWLockExclusive(&cv.lk);
    cv.gen++;
    ReleaseSRWLockExclusive(&cv.lk);
    WakeConditionVariable(&cv.cv);
}
static DWORD WINAPI consumer(void *p) {
    (void)p;
    for (;;) {
        EnterCriticalSection(&gm);
        while (!items && consumed < produced_total) wait_cv();
        if (items) { items--; consumed++; }
        const int fin = consumed >= produced_total;
        LeaveCriticalSection(&gm);
        if (fin) { AcquireSRWLockExclusive(&cv.lk); cv.gen++; ReleaseSRWLockExclusive(&cv.lk); WakeAllConditionVariable(&cv.cv); break; }
    }
    InterlockedIncrement(&done);
    return 0;
}
static DWORD WINAPI producer(void *p) {
    (void)p;
    for (int i = 0; i < produced_total / nprod; i++) {
        EnterCriticalSection(&gm);
        items++;
        signal_cv();   /* signal with the mutex held: the waiter's registration happens-before it */
        LeaveCriticalSection(&gm);
        if ((i & 255) == 0) Sleep(0);
    }
    InterlockedIncrement(&done);
    return 0;
}
static int run(int f) {
    fast = f; items = consumed = 0; done = 0; produced_total = 400000; cv.gen = 0; cv.waiting = 0;
    HANDLE h[16]; int n = 0;
    for (int i = 0; i < ncons; i++) h[n++] = CreateThread(NULL, 0, consumer, NULL, 0, NULL);
    for (int i = 0; i < nprod; i++) h[n++] = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    if (WaitForMultipleObjects(n, h, TRUE, 60000) == WAIT_TIMEOUT) { printf("FAIL: hang (a lost wake-up), fast=%d, consumed %d of %d\n", f, consumed, produced_total); return 1; }
    printf("ok fast=%d consumed %d\n", f, consumed);
    return consumed != produced_total;
}
int main(void) {
    InitializeCriticalSection(&gm);
    int bad = 0;
    for (int r = 0; r < 5; r++) bad |= run(0) | run(1);
    puts(bad ? "FAILED" : "PASSED");
    return bad;
}
