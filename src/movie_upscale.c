/*
 * Upscaled movies: movies\<name>.mp4 beside the exe is shown in place of the
 * title's <name>.bik (the Activision intro, upscaled x4 with Real-ESRGAN --
 * tools/movie_upscale.py).
 *
 * The title still plays its own Bink -- sound, timing, skipping with Start
 * all stay its own. It draws each Bink frame from one 640x480 texture,
 * rewritten in place; gpu_backend.c asks here for a picture whenever it
 * re-reads that texture, and gets the mp4's frame for the time since the
 * movie's first frame (Bink keeps real time, so the two stay together).
 * A thread decodes the mp4 with Media Foundation a few frames ahead, so the
 * render thread never waits on it.
 *
 * RECOMP_MOVIE_UPSCALE=0 turns it off.
 */
#define COBJMACROS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

extern void xbox_set_file_open_hook(void (*fn)(const WCHAR *path));

#define AHEAD 4                         /* frames decoded ahead of need */

static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_wake;
static WCHAR s_path[MAX_PATH];          /* the mp4 for the .bik last opened */
static long  s_seq;                     /* bumped per .bik opened */

/* Shared with the decoder, under s_lock. */
static long     s_want = -1;            /* frame the renderer needs */
static long     s_slot_idx[AHEAD];      /* frame each slot holds, -1 none */
static uint32_t *s_slot[AHEAD];
static uint32_t s_w, s_h;
static double   s_fps;
static int      s_ready;                /* the decoder has this s_seq open */

/* The renderer's: when the movie's first frame was asked for. */
static long     s_t_seq = -1;
static LONGLONG s_t0, s_qpf;

static void on_open(const WCHAR *path)
{
    const WCHAR *name = wcsrchr(path, L'\\'), *dot;
    WCHAR mp4[MAX_PATH];
    size_t n;

    name = name ? name + 1 : path;
    dot = wcsrchr(name, L'.');
    if (!dot || _wcsicmp(dot, L".bik") != 0)
        return;
    n = GetModuleFileNameW(NULL, mp4, MAX_PATH);
    while (n && mp4[n - 1] != L'\\')
        n--;
    _snwprintf(mp4 + n, MAX_PATH - n, L"movies\\%.*s.mp4", (int)(dot - name), name);
    mp4[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(mp4) == INVALID_FILE_ATTRIBUTES)
        mp4[0] = 0;
    EnterCriticalSection(&s_lock);
    wcscpy(s_path, mp4);
    s_seq++;
    s_ready = 0;
    s_want = -1;
    LeaveCriticalSection(&s_lock);
    WakeConditionVariable(&s_wake);
    fprintf(stderr, "  [MOVIE] %ls: %ls\n", name, mp4[0] ? mp4 : L"the title's own");
}

static IMFSourceReader *reader_open(const WCHAR *path, uint32_t *w, uint32_t *h, double *fps)
{
    IMFSourceReader *rd = NULL;
    IMFAttributes *a = NULL;
    IMFMediaType *mt = NULL, *cur = NULL;
    UINT64 size = 0, rate = 0;
    HRESULT hr;

    hr = MFCreateAttributes(&a, 1);
    if (SUCCEEDED(hr))
        IMFAttributes_SetUINT32(a, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr))
        hr = MFCreateSourceReaderFromURL(path, a, &rd);
    if (a)
        IMFAttributes_Release(a);
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&mt);
    if (SUCCEEDED(hr)) {
        IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
        hr = IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                 NULL, mt);
        IMFMediaType_Release(mt);
    }
    if (SUCCEEDED(hr))
        hr = IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                 &cur);
    if (SUCCEEDED(hr)) {
        hr = IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &size);
        IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_RATE, &rate);
        IMFMediaType_Release(cur);
    }
    if (FAILED(hr) || !size) {
        fprintf(stderr, "  [MOVIE] %ls: cannot decode (0x%08lX)\n", path, (unsigned long)hr);
        if (rd)
            IMFSourceReader_Release(rd);
        return NULL;
    }
    *w = (uint32_t)(size >> 32);
    *h = (uint32_t)size;
    *fps = (rate >> 32) && (uint32_t)rate ? (double)(rate >> 32) / (uint32_t)rate : 29.97;
    return rd;
}

/* The reader's next frame into px (w x h, opaque). 0 at the end. */
static int reader_next(IMFSourceReader *rd, uint32_t *px, uint32_t w, uint32_t h)
{
    DWORD idx, flags = 0, len;
    LONGLONG ts;
    IMFSample *smp = NULL;
    IMFMediaBuffer *buf = NULL;
    BYTE *p;
    size_t i, n = (size_t)w * h;

    if (FAILED(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                          &idx, &flags, &ts, &smp))
        || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || !smp) {
        if (smp)
            IMFSample_Release(smp);
        return 0;
    }
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &buf))
        && SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
        if (len >= n * 4)
            for (i = 0; i < n; i++)
                px[i] = ((const uint32_t *)p)[i] | 0xFF000000u;
        IMFMediaBuffer_Unlock(buf);
    }
    if (buf)
        IMFMediaBuffer_Release(buf);
    IMFSample_Release(smp);
    return 1;
}

/* Decodes the current movie into the slots, AHEAD frames past s_want. */
static DWORD WINAPI decoder(LPVOID unused)
{
    IMFSourceReader *rd = NULL;
    long seq = -1, next = 0;            /* next: the frame the reader gives next */
    uint32_t w = 0, h = 0, *tmp = NULL;
    double fps = 0;
    int i;

    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    for (;;) {
        WCHAR path[MAX_PATH];
        long want, slot;

        EnterCriticalSection(&s_lock);
        while (seq == s_seq && (!rd || next > s_want + AHEAD - 1 + (s_want < 0)))
            SleepConditionVariableCS(&s_wake, &s_lock, INFINITE);
        if (seq != s_seq) {
            seq = s_seq;
            wcscpy(path, s_path);
            for (i = 0; i < AHEAD; i++)
                s_slot_idx[i] = -1;
            LeaveCriticalSection(&s_lock);
            if (rd)
                IMFSourceReader_Release(rd);
            rd = path[0] ? reader_open(path, &w, &h, &fps) : NULL;
            next = 0;
            EnterCriticalSection(&s_lock);
            if (rd && seq == s_seq) {
                if (s_w != w || s_h != h) {
                    for (i = 0; i < AHEAD; i++) {
                        free(s_slot[i]);
                        s_slot[i] = (uint32_t *)malloc((size_t)w * h * 4);
                        if (!s_slot[i]) {
                            IMFSourceReader_Release(rd);
                            rd = NULL;
                        }
                    }
                    free(tmp);
                    tmp = (uint32_t *)malloc((size_t)w * h * 4);
                    if (!tmp && rd) {
                        IMFSourceReader_Release(rd);
                        rd = NULL;
                    }
                    s_w = w;
                    s_h = h;
                }
                s_fps = fps;
                s_ready = rd != NULL;
            }
            LeaveCriticalSection(&s_lock);
            continue;
        }
        want = s_want < 0 ? 0 : s_want;
        LeaveCriticalSection(&s_lock);
        if (next < want)                /* fell behind: skip to the frame needed */
            while (next < want && reader_next(rd, tmp, w, h))
                next++;
        if (!reader_next(rd, tmp, w, h)) {
            IMFSourceReader_Release(rd);   /* the end: the last frames stay */
            rd = NULL;
            continue;
        }
        EnterCriticalSection(&s_lock);
        if (seq == s_seq) {
            slot = next % AHEAD;
            memcpy(s_slot[slot], tmp, (size_t)w * h * 4);
            s_slot_idx[slot] = next;
        }
        LeaveCriticalSection(&s_lock);
        next++;
    }
}

/* For gpu_backend.c, on the render thread: the frame of the replaced movie
 * now playing, or NULL to show the title's own. The pointer stays valid
 * until the next call. */
const uint32_t *movie_upscale_texture(uint32_t *w, uint32_t *h)
{
    static uint32_t *out;
    static size_t out_n;
    const uint32_t *ret = NULL;
    LARGE_INTEGER now;
    long n, i, best = -1;

    if (!s_qpf)
        return NULL;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&s_lock);
    if (!s_ready) {
        LeaveCriticalSection(&s_lock);
        return NULL;
    }
    if (s_t_seq != s_seq) {
        s_t_seq = s_seq;
        s_t0 = now.QuadPart;
    }
    n = (long)((double)(now.QuadPart - s_t0) / s_qpf * s_fps);
    s_want = n;
    /* The frame for now, else the latest decoded before it. */
    for (i = 0; i < AHEAD; i++)
        if (s_slot_idx[i] >= 0 && s_slot_idx[i] <= n
            && (best < 0 || s_slot_idx[i] > s_slot_idx[best]))
            best = i;
    if (best >= 0) {
        size_t sz = (size_t)s_w * s_h;
        if (out_n != sz) {
            free(out);
            out = (uint32_t *)malloc(sz * 4);
            out_n = out ? sz : 0;
        }
        if (out) {
            memcpy(out, s_slot[best], sz * 4);
            *w = s_w;
            *h = s_h;
            ret = out;
        }
    }
    LeaveCriticalSection(&s_lock);
    WakeConditionVariable(&s_wake);
    return ret;
}

void movie_upscale_init(void)
{
    const char *e = getenv("RECOMP_MOVIE_UPSCALE");
    LARGE_INTEGER f;
    HANDLE th;

    if (e && e[0] == '0')
        return;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        return;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_wake);
    th = CreateThread(NULL, 0, decoder, NULL, 0, NULL);
    if (!th)
        return;
    CloseHandle(th);
    QueryPerformanceFrequency(&f);
    s_qpf = f.QuadPart;
    xbox_set_file_open_hook(on_open);
}
