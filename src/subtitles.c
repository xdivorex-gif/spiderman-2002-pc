/*
 * Subtitles. The game ships no dialogue text, so subs.txt beside the exe holds
 * transcripts of its voice lines and movies (tools/subtitles.py, whisper.cpp).
 *
 * A voice line shows when the sound layer starts it (sub_002B3D00_enter,
 * recomp_manual.c, reads the sound's name out of the instance's slot) and stays
 * for the sample's length; a long one turns its screens ("|" in subs.txt) in
 * step with the speech, by their share of the text. A movie's lines run on the clock from its first
 * frame; the movie's end or a skip stops them, because its texture stops
 * changing. gpu_backend.c draws what subtitles_text() returns, each flip, in
 * the game's font. settings.ini [Game] Subtitles=0 turns them off; the pause
 * screen toggles them (costume_panel.c).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; float secs; const char *text; int chars; } VoiceLine;
typedef struct { const char *name; float a, b; const char *text; } MovieLine;

static VoiceLine *s_voice;
static MovieLine *s_movie;
static int s_nvoice, s_nmovie;
static volatile int s_on = 1;

/* The line showing. One pointer and two times: read on the render thread,
 * written on the game's; a line is constant. */
static const VoiceLine *volatile s_line;
static volatile ULONGLONG s_from, s_until;

/* The movie playing, set when its .bik is opened. */
static char s_movie_name[32];
static volatile LONG s_movie_wait;      /* opened, first frame not seen */
static volatile ULONGLONG s_movie_t0, s_movie_last;

static int by_voice(const void *a, const void *b)
{
    return _stricmp(((const VoiceLine *)a)->name, ((const VoiceLine *)b)->name);
}

static char *read_all(const char *rel, size_t *size)
{
    char path[MAX_PATH];
    size_t n = GetModuleFileNameA(NULL, path, MAX_PATH);
    FILE *f;
    char *d;
    long len;

    while (n && path[n - 1] != '\\')
        n--;
    _snprintf(path + n, MAX_PATH - n, "%s", rel);
    path[MAX_PATH - 1] = 0;
    if (!(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (char *)malloc((size_t)len + 1);
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (d)
        d[len] = 0;
    if (size)
        *size = (size_t)len;
    return d;
}

void subtitles_init(void)
{
    char *d = read_all("subs.txt", NULL), *p;
    const char *ini = getenv("RECOMP_INPUT_INI");
    int lines = 0;

    if (ini && *ini)
        s_on = GetPrivateProfileIntA("Game", "Subtitles", 1, ini) != 0;
    if (!d)
        return;
    for (p = d; *p; p++)
        lines += *p == '\n';
    s_voice = (VoiceLine *)calloc((size_t)lines + 1, sizeof *s_voice);
    s_movie = (MovieLine *)calloc((size_t)lines + 1, sizeof *s_movie);
    if (!s_voice || !s_movie)
        return;
    for (p = strtok(d, "\n"); p; p = strtok(NULL, "\n")) {
        char kind = p[0], *name = p + 2, *q = strchr(name, ' '), *r;
        if (!q || p[1] != ' ')
            continue;
        *q++ = 0;
        if (kind == 'v') {
            float secs = (float)strtod(q, &r);
            if (!*r)
                continue;                       /* no text after the time */
            s_voice[s_nvoice].name = name;
            s_voice[s_nvoice].secs = secs;
            s_voice[s_nvoice].text = r + 1;
            s_voice[s_nvoice].chars = (int)strlen(r + 1);
            for (r++; *r; r++)                  /* each screen its own string */
                if (*r == '|')
                    *r = 0;
            s_nvoice++;
        } else if (kind == 'm') {
            float a = (float)strtod(q, &r), b = (float)strtod(r, &r);
            if (!*r)
                continue;
            s_movie[s_nmovie].name = name;
            s_movie[s_nmovie].a = a;
            s_movie[s_nmovie].b = b;
            s_movie[s_nmovie].text = r + 1;
            s_nmovie++;
        }
    }
    qsort(s_voice, (size_t)s_nvoice, sizeof *s_voice, by_voice);
    fprintf(stderr, "  [SUBS] %d voice lines, %d movie lines, %s\n", s_nvoice, s_nmovie,
            s_on ? "on" : "off");
}

int subtitles_enabled(void)
{
    return s_on;
}

void subtitles_toggle(void)
{
    const char *ini = getenv("RECOMP_INPUT_INI");

    s_on = !s_on;
    if (ini && *ini)
        WritePrivateProfileStringA("Game", "Subtitles", s_on ? "1" : "0", ini);
}

/* The sound layer is starting the sound of this name (game thread). */
void subtitles_voice(const char *name)
{
    VoiceLine key, *hit;
    float secs;
    static int logging = -1;

    if (!s_nvoice)
        return;
    key.name = name;
    hit = (VoiceLine *)bsearch(&key, s_voice, (size_t)s_nvoice, sizeof *s_voice, by_voice);
    if (logging < 0)
        logging = getenv("RECOMP_SUBS_LOG") != NULL;
    if (logging)
        fprintf(stderr, "[SUBS] sound %.31s: %s\n", name, hit ? hit->text : "-");
    if (!hit)
        return;
    if (s_line == hit && GetTickCount64() < s_until)
        return;                         /* the same line, still showing */
    secs = hit->secs + 0.4f;            /* a moment to finish reading */
    if (secs < 1.6f)
        secs = 1.6f;
    s_from = GetTickCount64();
    s_until = s_from + (ULONGLONG)(secs * 1000.0f);
    s_line = hit;
}

/* The screen of a voice line due now. */
static const char *voice_screen(const VoiceLine *v, ULONGLONG now)
{
    const char *p = v->text, *end = v->text + v->chars;
    float at = (float)(now - s_from) / 1000.0f / v->secs * (float)v->chars;

    for (;;) {
        const char *q = p + strlen(p);
        if (q >= end || at < (float)(q - v->text))
            return p;
        p = q + 1;
    }
}

/* A .bik was opened: path without directory or extension. */
void subtitles_movie_opened(const char *name)
{
    _snprintf(s_movie_name, sizeof s_movie_name - 1, "%s", name);
    s_movie_name[sizeof s_movie_name - 1] = 0;
    InterlockedExchange(&s_movie_wait, 1);
    s_movie_last = 0;
}

/* A new Bink frame reached its texture (render thread). */
void subtitles_movie_frame(void)
{
    ULONGLONG now = GetTickCount64();

    if (InterlockedExchange(&s_movie_wait, 0))
        s_movie_t0 = now;
    s_movie_last = now;
}

/* A movie is showing: for placing the lines in its letterbox. */
int subtitles_in_movie(void)
{
    return s_movie_last && GetTickCount64() - s_movie_last < 1000;
}

/* What to show now, or NULL. */
const char *subtitles_text(void)
{
    ULONGLONG now = GetTickCount64();
    int i;

    if (!s_on)
        return NULL;
    if (subtitles_in_movie()) {
        float t = (float)(now - s_movie_t0) / 1000.0f;
        for (i = 0; i < s_nmovie; i++)
            if (t >= s_movie[i].a && t < s_movie[i].b + 0.2f
                    && _stricmp(s_movie[i].name, s_movie_name) == 0)
                return s_movie[i].text;
        return NULL;
    }
    return now < s_until && s_line ? voice_screen(s_line, now) : NULL;
}
