/*
 * book2audio - Convert ebooks (chapters in Markdown) to audiobooks (.m4b)
 *
 * Pipeline: text -> espeak-ng (IPA phonemes) -> Kokoro ONNX (C API) -> WAV -> ffmpeg -> M4B
 *
 * Complements import_book: it parses ebooks to /opt/books/{book}/chapters/xx/page_*.md,
 * this tool reads those pages and synthesizes speech.
 *
 * Modes:
 *   book2audio synth -t "Hello world" -o out.wav [options]
 *   book2audio file  -i chapter.txt   -o out.wav [options]
 *   book2audio book  -i /opt/books/ddia -o /opt/audio/ddia [options]
 *
 * Options:
 *   -v NAME      voice (default af_sky); voice file: <voices_dir>/<name>.bin
 *   -l LANG      espeak language (default en-us)
 *   -s SPEED     0.5 .. 2.0 (default 1.0)
 *   --phonemes   input is already IPA phonemes (skip espeak; for zh/ja frontends)
 *   --cpu        force CPU (default: try CUDA, fallback to CPU)
 *   --model P    ONNX model path
 *   --voices P   voices directory
 *   --vocab P    vocab.tsv path
 *   --no-m4b     (book mode) skip ffmpeg packaging
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>

#include "onnxruntime_c_api.h"

/* ---------------- defaults ---------------- */
#define DEFAULT_MODEL   "/data/models/kokoro/kokoro-v1.0.fp16.onnx"
#define DEFAULT_VOICES  "/data/models/kokoro/voices"
#define DEFAULT_VOCAB   "/data/models/kokoro/vocab.tsv"

#define SAMPLE_RATE     24000
#define MAX_PHONEMES    510
#define VOICE_ROWS      510
#define VOICE_DIM       256
#define MAX_VOCAB       256
#define SENT_PAUSE_S    0.25f
#define CLAUSE_PAUSE_S  0.10f

/* ---------------- globals ---------------- */
static const OrtApi *g_ort = NULL;

/* vocab: id -> utf8 char (up to 4 bytes + NUL) */
static char  g_vocab_chars[MAX_VOCAB][8];
static int   g_vocab_size = 0;

/* ---------------- utils ---------------- */
static void die(const char *msg) { fprintf(stderr, "Error: %s\n", msg); exit(1); }

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* length in bytes of the UTF-8 char starting at s */
static int utf8_char_len(const char *s) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* ---------------- vocab ---------------- */
static void load_vocab(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Error: cannot open vocab %s\n", path); exit(1); }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        int id = atoi(line);
        if (id < 0 || id >= MAX_VOCAB) continue;
        *tab++ = 0;
        char *nl = strchr(tab, '\n'); if (nl) *nl = 0;
        snprintf(g_vocab_chars[id], 8, "%s", tab);
        if (id + 1 > g_vocab_size) g_vocab_size = id + 1;
    }
    fclose(f);
    if (g_vocab_size < 10) die("vocab looks empty");
}

/* char (utf8, clen bytes) -> token id, or -1 if not in vocab */
static int vocab_lookup(const char *c, int clen) {
    for (int i = 0; i < g_vocab_size; i++) {
        const char *v = g_vocab_chars[i];
        if (v[0] == c[0] && (int)strlen(v) == clen && memcmp(v, c, clen) == 0)
            return i;
    }
    return -1;
}

static int is_sentence_mark(const char *c, int clen) {
    return (clen == 1 && (c[0] == '.' || c[0] == '!' || c[0] == '?')) ||
           (clen == 3 && memcmp(c, "\xE2\x80\xA6", 3) == 0); /* … */
}
static int is_clause_mark(const char *c, int clen) {
    return clen == 1 && (c[0] == ',' || c[0] == ';' || c[0] == ':');
}

/* ---------------- espeak G2P ---------------- */
/*
 * Split text into segments at sentence/clause punctuation (punct kept),
 * phonemize all segments in ONE espeak-ng invocation (one segment per line),
 * then re-attach punctuation. Mirrors phonemizer's preserve_punctuation.
 */
static char *g_segment_buf = NULL;   /* reused temp file path buffer */

static char *phonemize(const char *text, const char *lang) {
    /* write segments to temp file */
    char tmpf[256];
    snprintf(tmpf, sizeof(tmpf), "/tmp/book2audio_%d.txt", (int)getpid());
    FILE *tf = fopen(tmpf, "w");
    if (!tf) die("cannot create temp file");

    /* punct attached after each segment (actual utf8 chars, up to 4 bytes each) */
    size_t tlen = strlen(text);
    char *puncts = xmalloc((tlen / 4 + 8) * 8);
    size_t npunct = 0;

    const char *seg_start = text;
    const char *p = text;
    while (*p) {
        int cl = utf8_char_len(p);
        if (is_sentence_mark(p, cl) || is_clause_mark(p, cl)) {
            /* write segment [seg_start, p) as one line */
            for (const char *q = seg_start; q < p; q++) {
                if (*q == '\n' || *q == '\r') fputc(' ', tf);
                else fputc(*q, tf);
            }
            fputc('\n', tf);
            memcpy(puncts + npunct * 8, p, cl);
            puncts[npunct * 8 + cl] = 0;
            npunct++;
            p += cl;
            seg_start = p;
        } else {
            p += cl;
        }
    }
    if (p > seg_start) {
        for (const char *q = seg_start; q < p; q++) {
            if (*q == '\n' || *q == '\r') fputc(' ', tf);
            else fputc(*q, tf);
        }
        fputc('\n', tf);
        puncts[npunct * 8] = 0; /* no trailing punct */
        npunct++;
    }
    fclose(tf);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "espeak-ng -q --ipa -v %s -f %s 2>/dev/null", lang, tmpf);
    FILE *ep = popen(cmd, "r");
    if (!ep) die("failed to run espeak-ng");

    size_t cap = tlen * 3 + 64, out_len = 0;
    char *out = xmalloc(cap);
    out[0] = 0;

    char line[8192];
    size_t seg_i = 0;
    while (fgets(line, sizeof(line), ep)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        if (out_len + l + 8 >= cap) { cap = (cap + l) * 2; out = realloc(out, cap); }
        memcpy(out + out_len, line, l); out_len += l;
        if (seg_i < npunct && puncts[seg_i * 8]) {
            size_t pl = strlen(puncts + seg_i * 8);
            memcpy(out + out_len, puncts + seg_i * 8, pl);
            out_len += pl;
        }
        out[out_len++] = ' ';
        out[out_len] = 0;
        seg_i++;
    }
    pclose(ep);
    remove(tmpf);
    free(puncts);
    return out;
}

/* normalize whitespace to single spaces + drop chars not in vocab */
static char *filter_vocab(const char *phonemes) {
    size_t n = strlen(phonemes);
    char *out = xmalloc(n + 1);
    size_t oi = 0;
    int last_space = 1;
    for (size_t i = 0; i < n; ) {
        int cl = utf8_char_len(phonemes + i);
        if (cl == 1 && isspace((unsigned char)phonemes[i])) {
            if (!last_space) out[oi++] = ' ';
            last_space = 1;
            i++;
            continue;
        }
        if (vocab_lookup(phonemes + i, cl) >= 0) {
            memcpy(out + oi, phonemes + i, cl);
            oi += cl;
            last_space = 0;
        }
        i += cl;
    }
    while (oi > 0 && out[oi-1] == ' ') oi--;
    out[oi] = 0;
    return out;
}

/* ---------------- phoneme chunking (port of kokoro-onnx chunker) ---------------- */
typedef struct { char **items; size_t n, cap; } strlist_t;

static void sl_push(strlist_t *l, const char *s, size_t len) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = realloc(l->items, l->cap * sizeof(char *));
    }
    char *c = xmalloc(len + 1);
    memcpy(c, s, len); c[len] = 0;
    l->items[l->n++] = c;
}

static void sl_free(strlist_t *l) {
    for (size_t i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL; l->n = l->cap = 0;
}

/* split s at positions: char in mark-set (level 0/1) followed by space, or any space (level 2).
 * level: 0 = sentence marks, 1 = clause marks, 2 = whitespace. Returns 1 if split happened. */
static int split_at_level(const char *s, int level, strlist_t *out) {
    size_t n = strlen(s);
    size_t start = 0;
    int found = 0;
    for (size_t i = 0; i < n; ) {
        int cl = utf8_char_len(s + i);
        int boundary = 0;
        if (level < 2) {
            int mark = (level == 0) ? is_sentence_mark(s + i, cl) : is_clause_mark(s + i, cl);
            if (mark && i + cl < n && isspace((unsigned char)s[i + cl])) boundary = 1;
        } else {
            if (cl == 1 && isspace((unsigned char)s[i])) boundary = 1;
        }
        if (boundary) {
            size_t end = (level < 2) ? i + cl : i; /* punctuation stays with preceding piece */
            if (end > start) sl_push(out, s + start, end - start);
            start = (level < 2) ? i + cl + 1 : i + 1;
            found = 1;
        }
        i += cl;
    }
    if (found && start < n) sl_push(out, s + start, n - start);
    return found;
}

static void atoms_rec(const char *s, int level, strlist_t *atoms) {
    size_t n = strlen(s);
    if ((int)n <= MAX_PHONEMES) {
        if (n > 0) sl_push(atoms, s, n);
        return;
    }
    for (int lv = level; lv < 3; lv++) {
        strlist_t pieces = {0};
        if (split_at_level(s, lv, &pieces) && pieces.n > 1) {
            for (size_t i = 0; i < pieces.n; i++) {
                /* strip */
                char *p = pieces.items[i];
                while (*p && isspace((unsigned char)*p)) p++;
                size_t l = strlen(p);
                while (l > 0 && isspace((unsigned char)p[l-1])) p[--l] = 0;
                if (l > 0) atoms_rec(p, lv + 1, atoms);
            }
            sl_free(&pieces);
            return;
        }
        sl_free(&pieces);
    }
    /* unbreakable long run: hard slice */
    for (size_t st = 0; st < n; st += MAX_PHONEMES) {
        size_t l = n - st;
        if (l > MAX_PHONEMES) l = MAX_PHONEMES;
        sl_push(atoms, s + st, l);
    }
}

/* group consecutive atoms into batches within limit; returns count, fills ranges [start,end) */
static int pack_ranges(const int *lengths, int n, int limit, int *ranges) {
    int nb = 0, start = 0, size = 0;
    for (int i = 0; i < n; i++) {
        int candidate = (i == start) ? lengths[i] : size + 1 + lengths[i];
        if (candidate > limit && i > start) {
            ranges[nb*2] = start; ranges[nb*2+1] = i; nb++;
            start = i; size = lengths[i];
        } else {
            size = candidate;
        }
    }
    if (n > 0) { ranges[nb*2] = start; ranges[nb*2+1] = n; nb++; }
    return nb;
}

static strlist_t split_phonemes(const char *phonemes) {
    strlist_t atoms = {0}, batches = {0};
    atoms_rec(phonemes, 0, &atoms);
    int n = (int)atoms.n;
    if (n == 0) return batches;

    int *lengths = xmalloc(n * sizeof(int));
    int max_len = 0;
    for (int i = 0; i < n; i++) {
        lengths[i] = (int)strlen(atoms.items[i]);
        if (lengths[i] > max_len) max_len = lengths[i];
    }

    int *ranges = xmalloc(n * 2 * sizeof(int));
    int fewest = pack_ranges(lengths, n, MAX_PHONEMES, ranges);

    /* binary search smallest limit that keeps the same batch count */
    int low = max_len, high = MAX_PHONEMES;
    while (low < high) {
        int mid = (low + high) / 2;
        if (pack_ranges(lengths, n, mid, ranges) <= fewest) high = mid;
        else low = mid + 1;
    }
    int nb = pack_ranges(lengths, n, low, ranges);

    for (int b = 0; b < nb; b++) {
        /* join atoms[start:end) with single spaces */
        size_t total = 0;
        for (int i = ranges[b*2]; i < ranges[b*2+1]; i++) total += strlen(atoms.items[i]) + 1;
        char *joined = xmalloc(total + 1);
        joined[0] = 0;
        for (int i = ranges[b*2]; i < ranges[b*2+1]; i++) {
            strcat(joined, atoms.items[i]);
            if (i + 1 < ranges[b*2+1]) strcat(joined, " ");
        }
        sl_push(&batches, joined, strlen(joined));
        free(joined);
    }
    free(lengths); free(ranges);
    sl_free(&atoms);
    return batches;
}

static float pause_after(const char *phonemes) {
    size_t n = strlen(phonemes);
    while (n > 0 && isspace((unsigned char)phonemes[n-1])) n--;
    if (n == 0) return 0;
    /* last char */
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)phonemes[i] & 0xC0) == 0x80) i--;
    int cl = utf8_char_len(phonemes + i);
    if (is_sentence_mark(phonemes + i, cl)) return SENT_PAUSE_S;
    if (is_clause_mark(phonemes + i, cl)) return CLAUSE_PAUSE_S;
    return 0;
}

/* ---------------- audio buffer ---------------- */
typedef struct { float *data; size_t n, cap; } audiobuf_t;

static void ab_append(audiobuf_t *a, const float *src, size_t n) {
    if (a->n + n > a->cap) {
        a->cap = (a->n + n) * 2;
        a->data = realloc(a->data, a->cap * sizeof(float));
    }
    memcpy(a->data + a->n, src, n * sizeof(float));
    a->n += n;
}
static void ab_silence(audiobuf_t *a, float seconds) {
    size_t n = (size_t)(seconds * SAMPLE_RATE);
    if (n == 0) return;
    if (a->n + n > a->cap) { a->cap = (a->n + n) * 2; a->data = realloc(a->data, a->cap * sizeof(float)); }
    memset(a->data + a->n, 0, n * sizeof(float));
    a->n += n;
}

/* trim leading/trailing silence (librosa trim, top_db=60, fl=2048, hop=512) */
static void trim_audio(const float *in, size_t n, size_t *start, size_t *end) {
    const int FL = 2048, HOP = 512;
    if (n < FL) { *start = 0; *end = n; return; }
    int nframes = (int)((n - FL) / HOP) + 1 + 2; /* +2 for center padding frames */
    /* pad n by FL/2 on both sides conceptually */
    float max_rms = 0;
    float *rms = xmalloc(nframes * sizeof(float));
    for (int f = 0; f < nframes; f++) {
        long off = (long)f * HOP - FL / 2;
        double acc = 0;
        for (int j = 0; j < FL; j++) {
            long idx = off + j;
            float v = (idx >= 0 && (size_t)idx < n) ? in[idx] : 0.0f;
            acc += (double)v * v;
        }
        rms[f] = (float)sqrt(acc / FL);
        if (rms[f] > max_rms) max_rms = rms[f];
    }
    if (max_rms <= 0) { *start = 0; *end = 0; free(rms); return; }
    float thresh = max_rms * powf(10.0f, -60.0f / 20.0f);
    int first = -1, last = -1;
    for (int f = 0; f < nframes; f++) if (rms[f] > thresh) { if (first < 0) first = f; last = f; }
    free(rms);
    if (first < 0) { *start = 0; *end = 0; return; }
    *start = (size_t)(first * HOP);
    *end = (size_t)((last + 1) * HOP);
    if (*end > n) *end = n;
}

/* ---------------- ONNX session ---------------- */
static OrtEnv *g_env = NULL;
static OrtSession *g_session = NULL;

static void ort_check(OrtStatus *st, const char *what) {
    if (st) {
        fprintf(stderr, "ONNX error (%s): %s\n", what, g_ort->GetErrorMessage(st));
        g_ort->ReleaseStatus(st);
        exit(1);
    }
}

static void session_init(const char *model_path, int use_cuda) {
    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    ort_check(g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "book2audio", &g_env), "env");
    OrtSessionOptions *so;
    ort_check(g_ort->CreateSessionOptions(&so), "session options");
    g_ort->SetIntraOpNumThreads(so, 4);
    OrtStatus *st = NULL;
    if (use_cuda) {
        OrtCUDAProviderOptions opts;
        memset(&opts, 0, sizeof(opts));
        opts.device_id = 0;
        st = g_ort->SessionOptionsAppendExecutionProvider_CUDA(so, &opts);
        if (st) {
            fprintf(stderr, "[warn] CUDA provider unavailable, using CPU\n");
            g_ort->ReleaseStatus(st);
            st = NULL;
        }
    }
    st = g_ort->CreateSession(g_env, model_path, so, &g_session);
    if (st && use_cuda) {
        /* retry without CUDA */
        g_ort->ReleaseStatus(st);
        fprintf(stderr, "[warn] session with CUDA failed, retrying on CPU\n");
        OrtSessionOptions *so2;
        ort_check(g_ort->CreateSessionOptions(&so2), "session options");
        g_ort->SetIntraOpNumThreads(so2, 4);
        st = g_ort->CreateSession(g_env, model_path, so2, &g_session);
        g_ort->ReleaseSessionOptions(so2);
    }
    ort_check(st, "create session");
    g_ort->ReleaseSessionOptions(so);
}

/* run one chunk: tokens (ntok ids) -> audio (appended to out). Returns samples produced. */
static size_t kokoro_infer(const int *tokens, int ntok, const float *style, float speed,
                           audiobuf_t *out, int do_trim, float pause_s) {
    /* build input tensor [1, ntok+2] with 0 padding */
    int64_t t = ntok + 2;
    int64_t *tok = xmalloc(t * sizeof(int64_t));
    tok[0] = 0;
    for (int i = 0; i < ntok; i++) tok[i+1] = tokens[i];
    tok[t-1] = 0;

    OrtMemoryInfo *mi;
    ort_check(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mi), "meminfo");

    OrtValue *inputs[3];
    int64_t tok_shape[2] = {1, t};
    ort_check(g_ort->CreateTensorWithDataAsOrtValue(mi, tok, t * sizeof(int64_t),
              tok_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &inputs[0]), "tokens tensor");

    int64_t style_shape[2] = {1, VOICE_DIM};
    ort_check(g_ort->CreateTensorWithDataAsOrtValue(mi, (void*)style, VOICE_DIM * sizeof(float),
              style_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[1]), "style tensor");

    int64_t speed_shape[1] = {1};
    ort_check(g_ort->CreateTensorWithDataAsOrtValue(mi, &speed, sizeof(float),
              speed_shape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputs[2]), "speed tensor");

    const char *in_names[] = {"tokens", "style", "speed"};
    const char *out_names[] = {"audio"};
    OrtValue *output = NULL;
    ort_check(g_ort->Run(g_session, NULL, in_names, (const OrtValue * const*)inputs, 3,
              out_names, 1, &output), "run");

    float *audio;
    ort_check(g_ort->GetTensorMutableData(output, (void**)&audio), "output data");
    OrtTensorTypeAndShapeInfo *info;
    ort_check(g_ort->GetTensorTypeAndShape(output, &info), "shape info");
    size_t n_samples;
    ort_check(g_ort->GetTensorShapeElementCount(info, &n_samples), "element count");
    g_ort->ReleaseTensorTypeAndShapeInfo(info);

    size_t s0 = 0, s1 = n_samples;
    if (do_trim) trim_audio(audio, n_samples, &s0, &s1);
    if (s1 > s0) ab_append(out, audio + s0, s1 - s0);
    if (do_trim && pause_s > 0) ab_silence(out, pause_s);

    g_ort->ReleaseValue(output);
    for (int i = 0; i < 3; i++) g_ort->ReleaseValue(inputs[i]);
    g_ort->ReleaseMemoryInfo(mi);
    free(tok);
    return s1 - s0;
}

/* ---------------- synthesize text -> audio ---------------- */
static size_t synthesize(const char *text, const char *lang, const float *voice,
                         float speed, int is_phonemes, audiobuf_t *out) {
    char *phonemes_raw = is_phonemes ? strdup(text) : phonemize(text, lang);
    char *phonemes = filter_vocab(phonemes_raw);
    free(phonemes_raw);
    if (strlen(phonemes) == 0) { free(phonemes); return 0; }

    strlist_t batches = split_phonemes(phonemes);
    size_t produced = 0;
    for (size_t b = 0; b < batches.n; b++) {
        const char *ph = batches.items[b];
        /* tokenize (count UTF-8 chars, all are in vocab after filter) */
        int tokens[MAX_PHONEMES + 8];
        int ntok = 0;
        for (size_t i = 0; ph[i] && ntok < MAX_PHONEMES; ) {
            int cl = utf8_char_len(ph + i);
            int id = vocab_lookup(ph + i, cl);
            if (id >= 0) tokens[ntok++] = id;
            i += cl;
        }
        if (ntok == 0) continue;
        int style_row = ntok < VOICE_ROWS ? ntok : VOICE_ROWS;
        const float *style = voice + (size_t)(style_row - 1) * VOICE_DIM;
        float pause = (b + 1 < batches.n) ? pause_after(ph) : 0;
        produced += kokoro_infer(tokens, ntok, style, speed, out, 1, pause);
    }
    sl_free(&batches);
    free(phonemes);
    return produced;
}

/* ---------------- WAV ---------------- */
static void write_wav(const char *path, const float *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Error: cannot write %s\n", path); return; }
    uint32_t data_size = (uint32_t)(n * 2);
    uint32_t riff_size = 36 + data_size;
    fwrite("RIFF", 1, 4, f); fwrite(&riff_size, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16; fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_fmt = 1, channels = 1, bits = 16;
    uint32_t rate = SAMPLE_RATE, byte_rate = SAMPLE_RATE * 2;
    uint16_t block_align = 2;
    fwrite(&audio_fmt, 2, 1, f); fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data_size, 4, 1, f);
    for (size_t i = 0; i < n; i++) {
        float v = data[i];
        if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

/* ---------------- voice ---------------- */
static float *load_voice(const char *voices_dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.bin", voices_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: voice not found: %s\n", path); exit(1); }
    float *v = xmalloc(VOICE_ROWS * VOICE_DIM * sizeof(float));
    size_t rd = fread(v, sizeof(float), VOICE_ROWS * VOICE_DIM, f);
    fclose(f);
    if (rd != VOICE_ROWS * VOICE_DIM) die("voice file truncated");
    return v;
}

/* ---------------- markdown cleaning ---------------- */
/* extract readable text from a markdown page: strip html comments, images, heading marks */
static char *md_to_text(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = xmalloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = 0;
    fclose(f);

    char *out = xmalloc(sz + 2);
    size_t oi = 0;
    int in_comment = 0, in_frontmatter = 0, line_no = 0;
    char *saveptr;
    for (char *line = strtok_r(buf, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        line_no++;
        /* strip html comments (possibly multi-line) */
        char *c;
        while ((c = strstr(line, in_comment ? "-->" : "<!--")) != NULL) {
            if (in_comment) { in_comment = 0; memmove(line, c + 3, strlen(c + 3) + 1); }
            else {
                char *e = strstr(c + 4, "-->");
                if (e) memmove(c, e + 3, strlen(e + 3) + 1);
                else { *c = 0; in_comment = 1; }
            }
        }
        if (in_comment) continue;
        /* front matter: --- ... --- at very start of file */
        if (line_no == 1 && strcmp(line, "---") == 0) { in_frontmatter = 1; continue; }
        if (in_frontmatter) {
            if (strcmp(line, "---") == 0) in_frontmatter = 0;
            continue;
        }
        /* skip images and horizontal rules */
        if (strncmp(line, "![", 2) == 0) continue;
        if (strcmp(line, "---") == 0) continue;
        /* strip heading marks */
        char *p = line;
        while (*p == '#') p++;
        if (p != line && *p == ' ') p++;
        if (*p == 0) continue;
        size_t l = strlen(p);
        memcpy(out + oi, p, l); oi += l;
        out[oi++] = ' ';
    }
    out[oi] = 0;
    free(buf);
    return out;
}

/* ---------------- directory helpers ---------------- */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static char **list_dir(const char *path, int want_dir, const char *suffix, size_t *count) {
    DIR *d = opendir(path);
    if (!d) { *count = 0; return NULL; }
    char **names = NULL; size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (want_dir && !S_ISDIR(st.st_mode)) continue;
        if (!want_dir && S_ISDIR(st.st_mode)) continue;
        if (suffix && !strstr(e->d_name, suffix)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; names = realloc(names, cap * sizeof(char*)); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char*), cmp_str);
    *count = n;
    return names;
}

static char *read_chapter_text(const char *chapter_dir) {
    size_t npages;
    char **pages = list_dir(chapter_dir, 0, ".md", &npages);
    char *text = NULL; size_t tlen = 0;
    for (size_t i = 0; i < npages; i++) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", chapter_dir, pages[i]);
        char *t = md_to_text(full);
        if (t && strlen(t) > 0) {
            size_t l = strlen(t);
            text = realloc(text, tlen + l + 2);
            memcpy(text + tlen, t, l);
            tlen += l;
            text[tlen++] = ' ';
            text[tlen] = 0;
        }
        free(t);
        free(pages[i]);
    }
    free(pages);
    return text;
}

/* ---------------- book mode ---------------- */
static int cmd_book(const char *book_dir, const char *out_dir, const char *voice_name,
                    const char *lang, float speed, const char *voices_dir,
                    int use_cuda, int make_m4b, int is_phonemes, const char *model_path,
                    const char *vocab_path) {
    char chapters_dir[1024];
    snprintf(chapters_dir, sizeof(chapters_dir), "%s/chapters", book_dir);
    size_t nch;
    char **chapters = list_dir(chapters_dir, 1, NULL, &nch);
    if (nch == 0) { fprintf(stderr, "Error: no chapters in %s\n", chapters_dir); return 1; }

    mkdir_p(out_dir);
    load_vocab(vocab_path);
    float *voice = load_voice(voices_dir, voice_name);
    session_init(model_path, use_cuda);

    /* book title from _meta.json (naive parse) */
    char title[256] = "";
    char meta_path[1024];
    snprintf(meta_path, sizeof(meta_path), "%s/_meta.json", book_dir);
    FILE *mf = fopen(meta_path, "r");
    if (mf) {
        char mbuf[4096]; size_t r = fread(mbuf, 1, sizeof(mbuf)-1, mf); mbuf[r] = 0; fclose(mf);
        char *tp = strstr(mbuf, "\"title\"");
        if (tp) {
            tp = strchr(tp + 7, ':'); if (tp) tp = strchr(tp, '"');
            if (tp) { char *e = strchr(tp + 1, '"');
                if (e) { size_t l = e - tp - 1; if (l >= sizeof(title)) l = sizeof(title)-1;
                    memcpy(title, tp + 1, l); title[l] = 0; } }
        }
    }
    const char *base = strrchr(book_dir, '/');
    base = base ? base + 1 : book_dir;
    if (title[0] == 0) snprintf(title, sizeof(title), "%s", base);

    /* concat list + ffmetadata for m4b */
    char list_path[1024], ffmeta_path[1024];
    snprintf(list_path, sizeof(list_path), "%s/_concat.txt", out_dir);
    snprintf(ffmeta_path, sizeof(ffmeta_path), "%s/_ffmeta.txt", out_dir);
    FILE *lf = fopen(list_path, "w");
    FILE *ff = make_m4b ? fopen(ffmeta_path, "w") : NULL;
    if (ff) { fprintf(ff, ";FFMETADATA1\ntitle=%s\n", title); }

    double total_s = 0;
    for (size_t c = 0; c < nch; c++) {
        char cdir[1024];
        snprintf(cdir, sizeof(cdir), "%s/%s", chapters_dir, chapters[c]);
        char *text = read_chapter_text(cdir);
        if (!text || strlen(text) < 5) {
            printf("[skip] %s (empty)\n", chapters[c]);
            free(text); continue;
        }
        audiobuf_t audio = {0};
        size_t samples = synthesize(text, lang, voice, speed, is_phonemes, &audio);
        double dur = (double)samples / SAMPLE_RATE;
        if (samples > 0) {
            char wav_path[1024];
            snprintf(wav_path, sizeof(wav_path), "%s/%s.wav", out_dir, chapters[c]);
            write_wav(wav_path, audio.data, audio.n);
            fprintf(lf, "file '%s.wav'\n", chapters[c]);
            printf("[ok] %s: %.1fs audio, %zu chars\n", chapters[c], dur, strlen(text));
            if (ff) {
                /* strip trailing slash-ish; chapter title = dir name */
                fprintf(ff, "[CHAPTER]\nTIMEBASE=1/1000\nSTART=%lld\nEND=%lld\ntitle=%s\n",
                        (long long)(total_s * 1000), (long long)((total_s + dur) * 1000),
                        chapters[c]);
            }
            total_s += dur;
        } else {
            printf("[skip] %s (no phonemes)\n", chapters[c]);
        }
        free(audio.data);
        free(text);
    }
    fclose(lf);

    if (make_m4b && total_s > 0) {
        fclose(ff);
        char m4b_path[1024];
        snprintf(m4b_path, sizeof(m4b_path), "%s/%s.m4b", out_dir, base);
        char cmd[4096];
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -f concat -safe 0 -i '%s' -i '%s' -map_metadata 1 -c:a aac -b:a 64k '%s' 2>/dev/null",
            list_path, ffmeta_path, m4b_path);
        int rc = system(cmd);
        if (rc == 0) printf("[done] %s (%.1f minutes)\n", m4b_path, total_s / 60.0);
        else fprintf(stderr, "[warn] ffmpeg failed; wav files are in %s\n", out_dir);
    }
    for (size_t c = 0; c < nch; c++) free(chapters[c]);
    free(chapters);
    free(voice);
    return 0;
}

/* ---------------- single text / file mode ---------------- */
static int cmd_single(const char *text, const char *out_wav, const char *voice_name,
                      const char *lang, float speed, const char *voices_dir,
                      int use_cuda, int is_phonemes, const char *model_path,
                      const char *vocab_path) {
    load_vocab(vocab_path);
    float *voice = load_voice(voices_dir, voice_name);
    session_init(model_path, use_cuda);
    audiobuf_t audio = {0};
    size_t samples = synthesize(text, lang, voice, speed, is_phonemes, &audio);
    if (samples == 0) { fprintf(stderr, "Error: no audio produced\n"); return 1; }
    write_wav(out_wav, audio.data, audio.n);
    printf("[done] %s: %.2fs audio\n", out_wav, (double)samples / SAMPLE_RATE);
    free(audio.data);
    free(voice);
    return 0;
}

/* ---------------- main ---------------- */
static void usage(void) {
    fprintf(stderr,
        "book2audio - text/markdown ebooks to audiobook (Kokoro TTS, ONNX)\n\n"
        "  book2audio synth -t \"Hello world\" -o out.wav [opts]\n"
        "  book2audio file  -i input.txt      -o out.wav [opts]\n"
        "  book2audio book  -i /opt/books/xxx -o /opt/audio/xxx [opts]\n\n"
        "opts: -v voice (af_sky)  -l lang (en-us)  -s speed (1.0)\n"
        "      --phonemes  --cpu  --no-m4b  --model P  --voices P  --vocab P\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) usage();
    const char *mode = argv[1];
    const char *text = NULL, *input = NULL, *output = NULL;
    const char *voice = "af_sky", *lang = "en-us";
    const char *model_path = DEFAULT_MODEL, *voices_dir = DEFAULT_VOICES, *vocab_path = DEFAULT_VOCAB;
    float speed = 1.0f;
    int use_cuda = 1, make_m4b = 1, is_phonemes = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) text = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) voice = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) lang = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) speed = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model_path = argv[++i];
        else if (!strcmp(argv[i], "--voices") && i + 1 < argc) voices_dir = argv[++i];
        else if (!strcmp(argv[i], "--vocab") && i + 1 < argc) vocab_path = argv[++i];
        else if (!strcmp(argv[i], "--cpu")) use_cuda = 0;
        else if (!strcmp(argv[i], "--no-m4b")) make_m4b = 0;
        else if (!strcmp(argv[i], "--phonemes")) is_phonemes = 1;
        else usage();
    }
    if (speed < 0.5f || speed > 2.0f) die("speed must be 0.5..2.0");

    if (!strcmp(mode, "synth")) {
        if (!text || !output) usage();
        return cmd_single(text, output, voice, lang, speed, voices_dir, use_cuda, is_phonemes, model_path, vocab_path);
    } else if (!strcmp(mode, "file")) {
        if (!input || !output) usage();
        FILE *f = fopen(input, "r");
        if (!f) { fprintf(stderr, "Error: cannot open %s\n", input); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = xmalloc(sz + 1);
        fread(buf, 1, sz, f); buf[sz] = 0; fclose(f);
        int rc = cmd_single(buf, output, voice, lang, speed, voices_dir, use_cuda, is_phonemes, model_path, vocab_path);
        free(buf);
        return rc;
    } else if (!strcmp(mode, "book")) {
        if (!input || !output) usage();
        return cmd_book(input, output, voice, lang, speed, voices_dir, use_cuda, make_m4b, is_phonemes, model_path, vocab_path);
    }
    usage();
    return 1;
}
