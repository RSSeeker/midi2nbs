/*
 * midi2nbs - 将 MIDI (.mid/.midi) 转换为 Minecraft 音符盒 (OpenNBS .nbs) 文件
 *
 * 纯 C 实现，无任何外部依赖，仅使用 C 标准库（Windows 上额外使用 Win32 控制台 API 设置 UTF-8 输出）。
 *
 * 用法:
 *   把 .mid 文件拖到 midi2nbs.exe 上，即可在 MIDI 同目录生成同名 .nbs 文件。
 *   命令行: midi2nbs.exe <歌曲.mid> [曲速 tick/s] [模式]
 *     曲速: 1-100，默认 10 tick/s
 *     模式: pitch（默认，按音高六八度分配乐器）| auto（按轨道分配）| 0-15（固定乐器）
 *
 * 转换规则与 mcbot (https://github.com/RSSeeker/mcbot-nodejs) 的 scripts/midi2nbs.js 一致:
 *   - 内置 SMF 解析器（PPQ 制），支持 tempo 变化事件
 *   - 实体 key 折叠到可发声范围（33-57，F#3~F#5）
 *   - 鼓通道(9)按鼓音高映射
 *   - 同 tick 同乐器同音高去重；同 tick 多音符自动分层（最多 40 层）
 *   - 开头静音自动裁剪
 *   - 输出 NBS v6 格式（与 @nbsjs/core 一致）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <setjmp.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define IS_TTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define IS_TTY(f) isatty(fileno(f))
#endif

#define MAX_TRACKS 512
#define MAX_LAYERS 40
#define NBS_VERSION 6
#define FIRST_CUSTOM_INDEX 20

/* ---------------- 转换参数（与 midi_common.js 一致） ---------------- */

#define MIN_PITCH 54   /* F#3 */
#define MAX_PITCH 78   /* F#5 */
#define NBS_KEY_BASE 21
#define SOUND_KEY_MIN 9    /* F#1 */
#define SOUND_KEY_MAX 81   /* F#7 */

static int drum_map(int p)
{
    switch (p) {
    case 35: case 36: return 2;  /* 底鼓 -> Bass Drum */
    case 38: case 40: return 3;  /* 军鼓 -> Snare Drum */
    case 42: case 44: case 46: return 4;  /* 踩镲 -> Click */
    case 49: case 51: case 57: return 9;  /* 打击乐 -> Xylophone */
    case 39: case 41: return 5;  /* 拍手/沙锤 -> Guitar */
    default: return 3;
    }
}

static int fold_pitch(int p)
{
    while (p < MIN_PITCH) p += 12;
    while (p > MAX_PITCH) p -= 12;
    return p;
}

static int instrument_for_pitch(int p)
{
    if (p <= 47) return 1;  /* 低音区 -> Double Bass */
    if (p <= 71) return 0;  /* 标准区 -> Harp */
    if (p <= 95) return 6;  /* 高音区 -> Flute */
    return 7;               /* 超高音区 -> Bell */
}

static int program_to_instrument(int prog)
{
    if (prog <= 7)  return 0;
    if (prog <= 23) return 0;
    if (prog <= 31) return 5;
    if (prog <= 39) return 1;
    if (prog <= 47) return 6;
    if (prog <= 55) return 0;
    if (prog <= 63) return 7;
    return 0;
}

static int fold_sound_key(int s)
{
    while (s < SOUND_KEY_MIN) s += 12;
    while (s > SOUND_KEY_MAX) s -= 12;
    return s;
}

/* pitch 模式：按音高六八度分配乐器，key 折叠到 33-57 */
static void assign_six_octave(int midiPitch, int *inst, int *key)
{
    int S = fold_sound_key(midiPitch - NBS_KEY_BASE);
    int i;
    if (S >= 33 && S <= 57)      i = 0;  /* F#3-F#5 -> Harp */
    else if (S >= SOUND_KEY_MIN && S < 33) i = 1;  /* F#1-F#3 -> Double Bass */
    else if (S > 57 && S <= 69)  i = 6;  /* F#5-F#6 -> Flute */
    else if (S > 69 && S <= SOUND_KEY_MAX) i = 7;  /* F#6-F#7 -> Bell */
    else i = 0;

    int offset = 0;
    switch (i) {
    case 1: offset = -24; break;
    case 5: offset = -12; break;
    case 6: offset =  12; break;
    case 7: offset =  24; break;
    default: offset = 0; break;
    }
    int k = S - offset;
    if (k < 33) k = 33;
    if (k > 57) k = 57;
    *inst = i;
    *key = k;
}

/* ---------------- MIDI (SMF) 解析 ---------------- */

typedef struct {
    uint32_t tick;
    uint8_t kind;   /* 0=note 1=tempo 2=program */
    uint8_t channel;
    uint8_t pitch;
    uint8_t velocity;
    uint8_t program;
    uint32_t tempo; /* 微秒/四分音符 */
} MEvent;

typedef struct {
    MEvent *items;
    size_t len, cap;
} Track;

typedef struct {
    Track tracks[MAX_TRACKS];
    int ntrks;
    uint16_t division;
} Smf;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} Reader;

static void fatal(const char *msg);

static uint8_t rd_u8(Reader *r)
{
    if (r->p >= r->end) fatal("MIDI 文件截断");
    return *r->p++;
}

static uint16_t rd_u16(Reader *r)
{
    return (uint16_t)(((uint16_t)rd_u8(r) << 8) | rd_u8(r));
}

static uint32_t rd_u32(Reader *r)
{
    return ((uint32_t)rd_u8(r) << 24) | ((uint32_t)rd_u8(r) << 16) |
           ((uint32_t)rd_u8(r) << 8) | rd_u8(r);
}

static uint32_t rd_vlq(Reader *r)
{
    uint32_t value = 0;
    uint8_t b;
    do {
        b = rd_u8(r);
        value = (value << 7) | (b & 0x7f);
    } while (b & 0x80);
    return value;
}

static void track_push(Track *tr, MEvent ev)
{
    if (tr->len == tr->cap) {
        tr->cap = tr->cap ? tr->cap * 2 : 64;
        tr->items = (MEvent *)realloc(tr->items, tr->cap * sizeof(MEvent));
        if (!tr->items) fatal("内存不足");
    }
    tr->items[tr->len++] = ev;
}

static Smf parse_smf(const uint8_t *buf, size_t size)
{
    Smf smf;
    memset(&smf, 0, sizeof(smf));
    Reader r = { buf, buf + size };

    if (size < 14 || memcmp(r.p, "MThd", 4) != 0) fatal("不是有效的 MIDI 文件（缺少 MThd 头）");
    r.p += 4;
    uint32_t hdrLen = rd_u32(&r);
    (void)rd_u16(&r);       /* format */
    uint16_t ntrks = rd_u16(&r);
    uint16_t division = rd_u16(&r);
    if (hdrLen < 6) fatal("MIDI 头长度无效");
    if (hdrLen > 6) r.p += hdrLen - 6;
    if (division & 0x8000) fatal("不支持 SMPTE 时间码格式的 MIDI（仅支持 PPQ）");
    if (division == 0) fatal("MIDI division 无效");
    smf.division = division;
    if (ntrks == 0) fatal("MIDI 没有音轨");
    if (ntrks > MAX_TRACKS) ntrks = MAX_TRACKS;

    for (int t = 0; t < ntrks; t++) {
        if (r.p + 8 > r.end) break;
        const char *id = (const char *)r.p;
        r.p += 4;
        uint32_t len = rd_u32(&r);
        const uint8_t *trkEnd = r.p + len;
        if (trkEnd > r.end) fatal("MIDI 音轨数据截断");
        if (memcmp(id, "MTrk", 4) != 0) {
            r.p = trkEnd;
            continue;
        }

        Track *tr = &smf.tracks[smf.ntrks++];
        uint32_t tick = 0;
        uint8_t running = 0;
        while (r.p < trkEnd) {
            tick += rd_vlq(&r);
            uint8_t status = rd_u8(&r);
            if (!(status & 0x80)) {
                r.p--;              /* running status */
                status = running;
            } else {
                running = status;
            }

            if (status == 0xff) {
                uint8_t metaType = rd_u8(&r);
                uint32_t dlen = rd_vlq(&r);
                if (metaType == 0x51 && dlen >= 3) {
                    MEvent volatile ev;
                    ev.tick = tick;
                    ev.kind = 1;
                    ev.tempo = ((uint32_t)rd_u8(&r) << 16) | ((uint32_t)rd_u8(&r) << 8) | rd_u8(&r);
                    track_push(tr, ev);
                    r.p += dlen - 3;
                } else {
                    r.p += dlen;
                }
                if (metaType == 0x2f) break;
            } else if ((status & 0xf0) == 0xf0) {
                r.p += rd_vlq(&r);  /* sysex，跳过 */
            } else {
                uint8_t channel = status & 0x0f;
                uint8_t kind = status & 0xf0;
                if (kind == 0x80 || kind == 0x90) {
                    uint8_t pitch = rd_u8(&r);
                    uint8_t velocity = rd_u8(&r);
                    if (kind == 0x90 && velocity > 0) {
                        MEvent volatile ev;
                        ev.tick = tick;
                        ev.kind = 0;
                        ev.channel = channel;
                        ev.pitch = pitch;
                        ev.velocity = velocity;
                        track_push(tr, ev);
                    }
                } else if (kind == 0xc0) {
                    MEvent ev;
                    ev.tick = tick;
                    ev.kind = 2;
                    ev.program = rd_u8(&r);
                    track_push(tr, ev);
                } else if (kind == 0xb0) {
                    rd_u8(&r); rd_u8(&r);
                } else {
                    rd_u8(&r); rd_u8(&r);
                }
            }
        }
    }
    if (smf.ntrks == 0) fatal("MIDI 中没有 MTrk 音轨");
    return smf;
}

/* ---------------- 轨道乐器选择（auto 模式） ---------------- */

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int pick_track_instrument(const Track *tr)
{
    int lastProgram = -1;
    int *pitches = NULL;
    size_t np = 0, cap = 0;

    for (size_t i = 0; i < tr->len; i++) {
        const MEvent *ev = &tr->items[i];
        if (ev->kind == 2) lastProgram = ev->program;
        else if (ev->kind == 0 && ev->channel != 9) {
            if (np == cap) {
                cap = cap ? cap * 2 : 32;
                pitches = (int *)realloc(pitches, cap * sizeof(int));
                if (!pitches) fatal("内存不足");
            }
            pitches[np++] = ev->pitch;
        }
    }
    if (lastProgram >= 0) {
        free(pitches);
        return program_to_instrument(lastProgram);
    }
    int inst = 0;
    if (np > 0) {
        qsort(pitches, np, sizeof(int), cmp_int);
        inst = instrument_for_pitch(pitches[np / 2]);
    }
    free(pitches);
    return inst;
}

/* ---------------- 合并排序 + tick -> 秒 ---------------- */

typedef struct {
    uint32_t tick;
    uint8_t kind;   /* 0=note 1=tempo */
    uint8_t channel;
    uint8_t pitch;
    uint8_t velocity;
    int trackInst;
    uint32_t tempo; /* 微秒/四分音符（tempo 事件） */
    uint64_t seq;   /* 插入顺序，保证稳定排序（JS Array.sort 是稳定的） */
    size_t origIdx; /* 在原始音符列表中的下标 */
    double seconds;
} MItem;

typedef struct {
    uint32_t tick;
    uint8_t channel;
    uint8_t pitch;
    uint8_t velocity;
    int trackInst;
    double seconds;
} NoteItem;

static int cmp_item(const void *a, const void *b)
{
    const MItem *x = (const MItem *)a, *y = (const MItem *)b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    if (x->kind != y->kind) return x->kind > y->kind ? -1 : 1;  /* tempo(1) 先于 note(0) */
    if (x->seq != y->seq) return x->seq < y->seq ? -1 : 1;       /* 保持原顺序 */
    return 0;
}

/* ---------------- 哈希表: tick -> 分层组 ---------------- */

typedef struct {
    uint8_t instrument;
    uint8_t key;
    uint8_t velocity;
} Note;

typedef struct {
    uint32_t tick;
    int count;                  /* 该 tick 已去重音符数（下一组的 idx） */
    int slots[MAX_LAYERS];      /* 层 -> notes[] 下标，-1 为空 */
} Group;

typedef struct {
    uint32_t tick;
    Group *group;
    int used;
} HEntry;

static HEntry *ht;
static size_t ht_cap, ht_count;

static uint64_t hash64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static void ht_init(size_t cap)
{
    ht_cap = cap;
    ht = (HEntry *)calloc(cap, sizeof(HEntry));
    if (!ht) fatal("内存不足");
    ht_count = 0;
}

static void ht_grow(void)
{
    size_t oldCap = ht_cap;
    HEntry *old = ht;
    ht_cap *= 2;
    ht = (HEntry *)calloc(ht_cap, sizeof(HEntry));
    if (!ht) fatal("内存不足");
    for (size_t i = 0; i < oldCap; i++) {
        if (!old[i].used) continue;
        size_t j = (size_t)hash64(old[i].tick) & (ht_cap - 1);
        while (ht[j].used) j = (j + 1) & (ht_cap - 1);
        ht[j] = old[i];
    }
    free(old);
}

static Group *ht_get_or_create(uint32_t tick)
{
    if ((ht_count + 1) * 10 >= ht_cap * 7) ht_grow();
    size_t i = (size_t)hash64(tick) & (ht_cap - 1);
    while (ht[i].used) {
        if (ht[i].tick == tick) return ht[i].group;
        i = (i + 1) & (ht_cap - 1);
    }
    Group *g = (Group *)calloc(1, sizeof(Group));
    if (!g) fatal("内存不足");
    g->tick = tick;
    for (int k = 0; k < MAX_LAYERS; k++) g->slots[k] = -1;
    ht[i].used = 1;
    ht[i].tick = tick;
    ht[i].group = g;
    ht_count++;
    return g;
}

static Note *notes;
static size_t notes_len, notes_cap;

static int notes_push(Note n)
{
    if (notes_len == notes_cap) {
        notes_cap = notes_cap ? notes_cap * 2 : 1024;
        notes = (Note *)realloc(notes, notes_cap * sizeof(Note));
        if (!notes) fatal("内存不足");
    }
    notes[notes_len] = n;
    return (int)notes_len++;
}

/* 去重集合: (tick<<16) | (inst<<8) | key */
static uint64_t *seen;
static size_t seen_cap;
static size_t seen_count;

static void seen_init(size_t cap)
{
    seen_cap = cap;
    seen_count = 0;
    seen = (uint64_t *)calloc(cap, sizeof(uint64_t));
    if (!seen) fatal("内存不足");
}

static void seen_grow(void)
{
    size_t oldCap = seen_cap;
    uint64_t *old = seen;
    seen_cap *= 2;
    seen = (uint64_t *)calloc(seen_cap, sizeof(uint64_t));
    if (!seen) fatal("内存不足");
    for (size_t i = 0; i < oldCap; i++) {
        if (!old[i]) continue;
        size_t j = (size_t)hash64(old[i]) & (seen_cap - 1);
        while (seen[j]) j = (j + 1) & (seen_cap - 1);
        seen[j] = old[i];
    }
    free(old);
}

static int seen_add(uint64_t k)
{
    if ((seen_count + 1) * 10 >= seen_cap * 7) seen_grow();
    size_t i = (size_t)hash64(k) & (seen_cap - 1);
    while (seen[i]) {
        if (seen[i] == k) return 0;
        i = (i + 1) & (seen_cap - 1);
    }
    seen[i] = k;
    seen_count++;
    return 1;
}

static int cmp_group(const void *a, const void *b)
{
    const Group *x = *(Group *const *)a, *y = *(Group *const *)b;
    return (x->tick > y->tick) - (x->tick < y->tick);
}

/* ---------------- NBS 写出 ---------------- */

static void w_u8(FILE *f, uint8_t v) { fputc(v, f); }
static void w_u16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void w_u32(FILE *f, uint32_t v)
{
    fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f);
    fputc((v >> 16) & 0xff, f); fputc((v >> 24) & 0xff, f);
}
static void w_str(FILE *f, const char *s)
{
    size_t n = strlen(s);
    w_u32(f, (uint32_t)n);
    fwrite(s, 1, n, f);
}

/* ---------------- 主转换 ---------------- */

static jmp_buf convert_env;
static const char *err_msg = NULL;

static void fatal(const char *msg)
{
    err_msg = msg;
    longjmp(convert_env, 1);
}

static void write_nbs(FILE *f, Group **groups, size_t ng,
                      int songLength, int layerCount, int nbsTempo)
{
    /* 头 */
    w_u16(f, 0);                 /* instruments 占位 */
    w_u8(f, NBS_VERSION);
    w_u8(f, FIRST_CUSTOM_INDEX);
    w_u16(f, (uint16_t)songLength);
    w_u16(f, (uint16_t)layerCount);
    w_str(f, "");                /* name */
    w_str(f, "");                /* author */
    w_str(f, "");                /* originalAuthor */
    w_str(f, "");                /* description */
    w_u16(f, (uint16_t)(nbsTempo * 100));
    w_u8(f, 0);                  /* autoSave enabled */
    w_u8(f, 10);                 /* autoSave interval */
    w_u8(f, 4);                  /* time signature */
    w_u32(f, 0); w_u32(f, 0); w_u32(f, 0); w_u32(f, 0); w_u32(f, 0);
    w_str(f, "");                /* importName */
    w_u8(f, 0);                  /* loop enabled */
    w_u8(f, 0);                  /* loop totalLoops */
    w_u16(f, 0);                 /* loop startTick */

    /* 音符数据 */
    int prevTick = -1;
    for (size_t i = 0; i < ng; i++) {
        Group *g = groups[i];
        w_u16(f, (uint16_t)((int)g->tick - prevTick));
        prevTick = (int)g->tick;
        int prevLayer = -1;
        for (int l = 0; l < MAX_LAYERS; l++) {
            int ni = g->slots[l];
            if (ni < 0) continue;
            Note *n = &notes[ni];
            w_u16(f, (uint16_t)(l - prevLayer));
            prevLayer = l;
            w_u8(f, n->instrument);
            w_u8(f, n->key);
            w_u8(f, n->velocity);
            w_u8(f, 100);        /* pan */
            w_u16(f, 0);         /* pitch */
        }
        w_u16(f, 0);
    }
    w_u16(f, 0);

    /* 层定义 */
    for (int i = 0; i < layerCount; i++) {
        w_str(f, "");
        w_u8(f, 0);              /* flags */
        w_u8(f, 100);            /* volume */
        w_u8(f, 100);            /* stereo */
    }

    /* 自定义乐器数量（全部使用内置乐器，所以为 0） */
    w_u8(f, 0);
}

/* 生成输出路径: 同目录同名 .nbs */
static int make_output_path(const char *inPath, char *outPath, size_t outCap)
{
    size_t len = strlen(inPath);
    if (len + 5 >= outCap) return 0;
    memcpy(outPath, inPath, len + 1);
    char *dot = NULL;
    for (char *c = outPath + len - 1; c >= outPath; c--) {
        if (*c == '\\' || *c == '/') break;
        if (*c == '.' && !dot) dot = c;
    }
    if (dot) {
        char ext[8] = {0};
        size_t el = 0;
        for (char *c = dot + 1; *c && el < 7; c++)
            ext[el++] = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
        if (strcmp(ext, "mid") == 0 || strcmp(ext, "midi") == 0)
            strcpy(dot, ".nbs");
        else
            strcat(outPath, ".nbs");
    } else {
        strcat(outPath, ".nbs");
    }
    return 1;
}

static int convert_file(const char *inPath, int nbsTempo, int mode, int fixedInst)
{
#define PUSH_ITEM(arr, n, cap, it) do { \
        if ((n) == (cap)) { \
            (cap) = (cap) ? (cap) * 2 : 256; \
            (arr) = (MItem *)realloc((arr), (cap) * sizeof(MItem)); \
            if (!(arr)) fatal("内存不足"); \
        } \
        (arr)[(n)++] = (it); \
    } while (0)

    FILE *in = fopen(inPath, "rb");
    if (!in) {
        fprintf(stderr, "错误: 无法打开文件 %s\n", inPath);
        return 0;
    }
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(in);
        fprintf(stderr, "错误: 文件为空或不可读: %s\n", inPath);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(in);
        fprintf(stderr, "错误: 内存不足\n");
        return 0;
    }
    if (fread(buf, 1, (size_t)sz, in) != (size_t)sz) {
        free(buf);
        fclose(in);
        fprintf(stderr, "错误: 读取文件失败: %s\n", inPath);
        return 0;
    }
    fclose(in);

    char outPath[4096];
    if (!make_output_path(inPath, outPath, sizeof(outPath))) {
        free(buf);
        fprintf(stderr, "错误: 路径过长: %s\n", inPath);
        return 0;
    }

    /* 错误统一走 longjmp，返回 0；成功后释放内存并返回 1 */
    /* longjmp 后这些变量的值可能不确定，标记 volatile 保证清理路径安全 */
    MItem *volatile items = NULL;
    NoteItem *volatile noteItems = NULL;
    size_t volatile nItems = 0, capItems = 0;
    size_t volatile nNoteItems = 0, capNoteItems = 0;
    Group **volatile groups = NULL;
    int ok = 0;
    uint64_t seq = 0;

    if (setjmp(convert_env) != 0) {
        fprintf(stderr, "错误: %s (%s)\n", err_msg, inPath);
        goto cleanup;
    }

    Smf smf = parse_smf(buf, (size_t)sz);

    /* 1. 收集 tempo 事件与音符事件 */
    for (int t = 0; t < smf.ntrks; t++) {
        Track *tr = &smf.tracks[t];
        int trackInst = (mode == 0) ? -1 : pick_track_instrument(tr);
        for (size_t i = 0; i < tr->len; i++) {
            MEvent *ev = &tr->items[i];
            if (ev->kind == 1) {
                MItem it;
                memset(&it, 0, sizeof(it));
                it.seq = seq++;
                it.tick = ev->tick;
                it.kind = 1;
                it.tempo = ev->tempo;
                PUSH_ITEM(items, nItems, capItems, it);
            } else if (ev->kind == 0) {
                MItem it;
                memset(&it, 0, sizeof(it));
                it.seq = seq++;
                it.tick = ev->tick;
                it.kind = 0;
                it.channel = ev->channel;
                it.pitch = ev->pitch;
                it.velocity = ev->velocity;
                it.trackInst = trackInst;
                it.origIdx = nNoteItems;
                PUSH_ITEM(items, nItems, capItems, it);

                NoteItem ni;
                ni.tick = ev->tick;
                ni.channel = ev->channel;
                ni.pitch = ev->pitch;
                ni.velocity = ev->velocity;
                ni.trackInst = trackInst;
                ni.seconds = 0;
                if (nNoteItems == capNoteItems) {
                    capNoteItems = capNoteItems ? capNoteItems * 2 : 256;
                    noteItems = (NoteItem *)realloc((NoteItem *)noteItems, capNoteItems * sizeof(NoteItem));
                    if (!noteItems) fatal("内存不足");
                }
                noteItems[nNoteItems++] = ni;
            }
        }
    }
    if (nNoteItems == 0) fatal("MIDI 里没有音符事件");

    /* 2. 按 tick 排序（同 tick tempo 在前），计算秒数 */
    qsort(items, nItems, sizeof(MItem), cmp_item);
    double seconds = 0;
    uint32_t lastTick = 0;
    uint32_t tempo = 500000;
    int hasNote = 0;
    for (size_t i = 0; i < nItems; i++) {
        MItem *it = &items[i];
        seconds += (double)(it->tick - lastTick) / smf.division * (tempo / 1e6);
        lastTick = it->tick;
        if (it->kind == 1) {
            tempo = it->tempo;
        } else {
            noteItems[it->origIdx].seconds = seconds;
            hasNote = 1;
        }
    }
    if (!hasNote) fatal("MIDI 里没有音符事件");

    /* 3. 裁剪开头静音 */
    double firstSec = noteItems[0].seconds;
    for (size_t i = 1; i < nNoteItems; i++)
        if (noteItems[i].seconds < firstSec)
            firstSec = noteItems[i].seconds;
    for (size_t i = 0; i < nNoteItems; i++)
        noteItems[i].seconds -= firstSec;

    /* 4. 解析音符 -> (tick, instrument, key, velocity)，去重并分层 */
    ht_init(1024);
    seen_init(4096);
    double maxSec = 0;
    int noteCount = 0;

    for (size_t i = 0; i < nNoteItems; i++) {
        NoteItem *it = &noteItems[i];
        if (it->seconds > maxSec) maxSec = it->seconds;

        long tickL = (long)llround(it->seconds * nbsTempo);
        int tick = tickL < 0 ? 0 : (int)tickL;
        int vel = (int)llround(it->velocity / 127.0 * 100.0);
        if (vel < 1) vel = 1;
        if (vel > 100) vel = 100;

        int inst, key;
        if (it->channel == 9) {
            inst = drum_map(it->pitch);
            key = fold_pitch(it->pitch) - NBS_KEY_BASE;
        } else if (mode == 0) {         /* pitch 模式（默认） */
            assign_six_octave(it->pitch, &inst, &key);
        } else {                        /* auto / 固定乐器 */
            if (fixedInst >= 0) inst = fixedInst;
            else if (it->trackInst >= 0) inst = it->trackInst;
            else inst = instrument_for_pitch(it->pitch);
            key = fold_pitch(it->pitch) - NBS_KEY_BASE;
        }

        uint64_t dk = ((uint64_t)(uint32_t)tick << 16) |
                      ((uint64_t)(uint8_t)inst << 8) | (uint64_t)(uint8_t)key;
        if (!seen_add(dk)) continue;    /* 同 tick 同乐器同音高去重 */

        Group *g = ht_get_or_create((uint32_t)tick);
        int idx = g->count++;
        int layer = idx < MAX_LAYERS ? idx : 0;
        Note n;
        n.instrument = (uint8_t)inst;
        n.key = (uint8_t)key;
        n.velocity = (uint8_t)vel;
        g->slots[layer] = notes_push(n);
        noteCount++;
    }
    if (noteCount == 0) fatal("转换后没有任何音符");

    /* 5. 计算歌曲长度与层数 */
    int songLength = 0;
    int layerCount = 0;
    for (size_t i = 0; i < ht_cap; i++) {
        if (!ht[i].used) continue;
        Group *g = ht[i].group;
        if ((int)g->tick > songLength) songLength = (int)g->tick;
        for (int l = 0; l < MAX_LAYERS; l++)
            if (g->slots[l] >= 0 && l + 1 > layerCount) layerCount = l + 1;
    }

    /* 6. 收集并按 tick 排序的组列表 */
    groups = (Group **)malloc(ht_count * sizeof(Group *));
    if (!groups) fatal("内存不足");
    size_t ng = 0;
    for (size_t i = 0; i < ht_cap; i++)
        if (ht[i].used) groups[ng++] = ht[i].group;
    qsort(groups, ng, sizeof(Group *), cmp_group);

    /* 7. 写 NBS 文件 */
    FILE *out = fopen(outPath, "wb");
    if (!out) fatal("无法写入输出文件");
    write_nbs(out, groups, ng, songLength, layerCount, nbsTempo);
    if (fclose(out) != 0) fatal("写入输出文件失败");

    printf("转换完成: %s（音符 %d 个 | %d 层 | 时长 %.1fs | 曲速 %d tick/s）\n",
           outPath, noteCount, layerCount, maxSec, nbsTempo);
    ok = 1;

cleanup:
    free(groups);
    free(items);
    free(noteItems);
    free(ht);
    free(seen);
    free(notes);
    free(buf);
    ht = NULL; seen = NULL; notes = NULL;
    return ok;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    /* 收集输入文件与可选参数 */
    const char *inputs[4096];
    int nInputs = 0;
    int nbsTempo = 10;
    int mode = 0;        /* 0=pitch 1=auto 2=fixed */
    int fixedInst = -1;
    int pauseMode = 0;   /* 默认转换完自动关闭窗口；--pause 可保留窗口 */

    if (argc < 2) {
        fprintf(stderr, "用法: 把 .mid 文件拖到本程序上，或运行:\n");
        fprintf(stderr, "  midi2nbs.exe <歌曲.mid> [曲速 1-100] [pitch|auto|0-15]\n");
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        FILE *probe = fopen(argv[i], "rb");
        if (probe) {
            fclose(probe);
            if (nInputs < 4096) inputs[nInputs++] = argv[i];
            continue;
        }
        /* 非文件参数: 曲速 / 模式 */
        char *end = NULL;
        long v = strtol(argv[i], &end, 10);
        if (end != argv[i] && *end == '\0' && !strchr(argv[i], '.')) {
            if (nbsTempo == 10 && v >= 1 && v <= 100) nbsTempo = (int)v;
            else if (v >= 0 && v <= 15 && fixedInst < 0) {
                fixedInst = (int)v;
                mode = 2;
            }
        } else if (strcmp(argv[i], "pitch") == 0) {
            mode = 0;
            fixedInst = -1;
        } else if (strcmp(argv[i], "auto") == 0) {
            mode = 1;
            fixedInst = -1;
        } else if (strcmp(argv[i], "--pause") == 0 ||
                   strcmp(argv[i], "--wait") == 0) {
            pauseMode = 1;
        }
    }

    if (nInputs == 0) {
        fprintf(stderr, "错误: 没有找到可转换的 MIDI 文件\n");
        return 2;
    }

    int allOk = 1;
    for (int i = 0; i < nInputs; i++) {
        if (!convert_file(inputs[i], nbsTempo, mode, fixedInst)) allOk = 0;
    }

    if (pauseMode && IS_TTY(stdin) && IS_TTY(stdout)) {
        printf("\n按回车键退出...");
        fflush(stdout);
        getchar();
    }
    return allOk ? 0 : 1;
}
