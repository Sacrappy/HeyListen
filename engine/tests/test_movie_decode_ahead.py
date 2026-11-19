"""The decode-ahead thread of host/nn_video.c (HeyListen): submissions are decoded into the queue by a thread of the
decoder's own, not on the game's AcquireXDecoderOutputBuffer.

The real decode_ahead / worker_wants / publish / worker_kick, cut from the file, run on real threads with a stand-in
decode_packet (a few ms per submission, one picture per packet, none for the end marker). Checks:

- the queue never holds more than `ahead` pictures while the reader is away (bounded memory), and the pictures come out
  in order, none lost or doubled, as the reader takes them;
- the stream's end is taken as soon as it is next, with the queue full: it marks the last picture, and only that one;
- a flush while a submission is being decoded: the thread's pictures are dropped, not published into the new stream;
- a stopped decoder (d->stop) ends the thread.

Windows toolchain (mingw clang): the file uses the Windows threading API (HeyListen's Linux build gets it from host/posix).
python -I -B tests/test_movie_decode_ahead.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def through(source, marker, last):
    start = source.index(marker)
    end = source.index("\n", source.index(last, start))
    return source[start:end]


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;
typedef struct AVPacket { int64_t pts; int size; } AVPacket;
struct SwsContext;
static void av_packet_free(AVPacket **p) { free(*p); *p = NULL; }
static int64_t hos_now_ns(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart); }
@TYPES@
static void picture_free(Picture *p) { free(p->rgba), free(p->nv12); memset(p, 0, sizeof *p); }
static int decode_ms = 3;
static volatile LONG release_decode = 1;   /* (0: a decoding waits for the test to let it go) */
static int decode_packet(Dec *d, AVPacket *pkt) {
    while (!release_decode) Sleep(1);
    Sleep(decode_ms);
    if (!pkt) return 1;
    Picture p = {0};
    p.w = p.h = 2, p.pts = pkt->pts;
    p.nv12 = (uint8_t *)calloc(1, 6);
    d->out[d->nout++] = p;
    return 0;
}
@FUNCS@

static int bad;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); bad = 1; } }
static void submit(Dec *d, int64_t pts, int end) {
    Pending *n = (Pending *)calloc(1, sizeof *n);
    if (!end) { n->pkt = (AVPacket *)calloc(1, sizeof(AVPacket)); n->pkt->pts = pts; }
    AcquireSRWLockExclusive(&d->lock);
    if (d->ptail) d->ptail->next = n; else d->phead = n;
    d->ptail = n;
    worker_kick(d);
    ReleaseSRWLockExclusive(&d->lock);
}
static void start(Dec *d, int ahead) {
    memset(d, 0, sizeof *d);
    InitializeSRWLock(&d->lock);
    InitializeConditionVariable(&d->cv_work);
    InitializeConditionVariable(&d->cv_pic);
    d->ahead = ahead;
    d->worker = CreateThread(NULL, 0, decode_ahead, d, 0, NULL);
}
static void stop(Dec *d) {
    AcquireSRWLockExclusive(&d->lock);
    d->stop = 1;
    WakeAllConditionVariable(&d->cv_work);
    ReleaseSRWLockExclusive(&d->lock);
    check(WaitForSingleObject(d->worker, 5000) == WAIT_OBJECT_0, "the thread ends when stopped");
    CloseHandle(d->worker);
    for (int i = 0; i < d->qn; i++) picture_free(&d->queue[(d->qhead + i) % MAX_QUEUE]);
}
/* the reader's take: the oldest picture, as dec_acquire does (lock, wait for one, pop, kick) */
static int take(Dec *d, int64_t *pts, int *last) {
    int got = 0;
    AcquireSRWLockExclusive(&d->lock);
    const int64_t until = hos_now_ns() + 2000000000;
    while (!d->qn && !d->eos && hos_now_ns() < until) SleepConditionVariableSRW(&d->cv_pic, &d->lock, 20, 0);
    if (d->qn) {
        *pts = d->queue[d->qhead].pts, *last = d->queue[d->qhead].last;
        picture_free(&d->queue[d->qhead]);
        d->qhead = (d->qhead + 1) % MAX_QUEUE, d->qn--;
        got = 1;
        worker_kick(d);
    }
    ReleaseSRWLockExclusive(&d->lock);
    return got;
}
int main(void) {
    Dec d;
    int64_t pts = 0;
    int last = 0;
    /* 1. the reader is away: the queue fills to `ahead` and no further */
    start(&d, 5);
    for (int i = 0; i < 30; i++) submit(&d, i, 0);
    Sleep(300);
    AcquireSRWLockExclusive(&d.lock);
    check(d.qn == 5, "the queue holds `ahead` pictures while the reader is away");
    check(d.phead != NULL, "the rest of the submissions wait, not decoded");
    ReleaseSRWLockExclusive(&d.lock);
    /* 2. the reader takes them: in order, none lost, the thread decodes on as room opens */
    int order_ok = 1, n = 0, max_q = 0;
    for (int i = 0; i < 30; i++) {
        if (!take(&d, &pts, &last)) { order_ok = 0; break; }
        if (pts != i) order_ok = 0;
        n++;
        AcquireSRWLockExclusive(&d.lock);
        if (d.qn > max_q) max_q = d.qn;
        ReleaseSRWLockExclusive(&d.lock);
    }
    check(n == 30 && order_ok, "30 pictures out in order, none lost");
    check(max_q <= 5, "never more than `ahead` queued");
    stop(&d);

    /* 3. the end with the queue full marks the last picture (and only it) */
    start(&d, 3);
    for (int i = 0; i < 3; i++) submit(&d, i, 0);
    submit(&d, 0, 1);
    Sleep(200);
    AcquireSRWLockExclusive(&d.lock);
    check(d.qn == 3 && d.eos && d.phead == NULL, "the end taken with the queue full");
    check(d.queue[(d.qhead + 2) % MAX_QUEUE].last && !d.queue[d.qhead].last && !d.queue[(d.qhead + 1) % MAX_QUEUE].last,
          "and only the last picture carries the flag");
    ReleaseSRWLockExclusive(&d.lock);
    stop(&d);

    /* 4. a flush while a submission is decoded: its picture is not published */
    start(&d, 4);
    release_decode = 0;
    submit(&d, 7, 0);
    for (;;) {
        AcquireSRWLockExclusive(&d.lock);
        const int decoding = d.decoding;
        ReleaseSRWLockExclusive(&d.lock);
        if (decoding) break;
        Sleep(1);
    }
    AcquireSRWLockExclusive(&d.lock);
    d.gen++;   /* (dec_flush: what is being decoded is stale) */
    ReleaseSRWLockExclusive(&d.lock);
    release_decode = 1;
    Sleep(100);
    AcquireSRWLockExclusive(&d.lock);
    check(d.qn == 0 && d.nout == 0 && !d.decoding, "a flushed decoding publishes nothing");
    ReleaseSRWLockExclusive(&d.lock);
    submit(&d, 9, 0);
    check(take(&d, &pts, &last) && pts == 9, "and the next stream's picture comes out");
    stop(&d);
    if (!bad) puts("ok");
    return bad;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=ROOT / "host/nn_video.c")
    args = parser.parse_args()
    if os.name != "nt":
        print("skipped: the Windows threading API (mingw) is needed")
        return
    src = args.source.read_text(encoding="utf-8")
    defines = "\n".join(re.findall(r"^#define (?:RESULT_NO_OUTPUT|POOL_MAGIC|DEC_MAGIC|MAX_SLOTS|MAX_QUEUE|MAX_OUT|"
                                   r"AHEAD_DEFAULT)\b.*$", src, re.M))
    types = "\n".join((defines,
                       through(src, "typedef struct {\n    int w, h, format, count;", "} Pool;"),
                       through(src, "typedef struct { uint8_t *rgba, *nv12;", "} Picture;"),
                       through(src, "typedef struct Pending {", "} Pending;"),
                       through(src, "typedef struct {\n    AVCodecContext *ctx;", "} Dec;")))
    funcs = "\n".join((block(src, "static void publish(Dec *d, int end) {") + "\n",
                       block(src, "static int worker_wants(const Dec *d) {") + "\n",
                       block(src, "static DWORD WINAPI decode_ahead(LPVOID arg) {") + "\n",
                       block(src, "static void worker_kick(Dec *d) {") + "\n"))
    fixture = HARNESS.replace("@TYPES@", types).replace("@FUNCS@", funcs)
    compiler = shutil.which(args.cxx)
    if not compiler:
        raise SystemExit("C++ compiler not found: " + args.cxx)
    with tempfile.TemporaryDirectory(prefix="decode-ahead-") as temporary:
        folder = Path(temporary)
        (folder / "fixture.cpp").write_text(fixture, encoding="utf-8")
        exe = folder / "fixture.exe"
        subprocess.run([compiler, "-std=c++17", "-O1", "-w", str(folder / "fixture.cpp"), "-o", str(exe)], check=True,
                       timeout=120)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        print(result.stdout, end="")
        if result.returncode or "ok" not in result.stdout:
            raise SystemExit("the decode-ahead thread does not keep its queue bounded and ordered")


if __name__ == "__main__":
    main()
