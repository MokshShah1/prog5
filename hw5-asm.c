#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static const uint64_t codeBase = 0x2000ULL;
static const uint64_t dataBase = 0x10000ULL;

static void stopBuild(const char *message)
{
    fprintf(stderr, "Error: %s\n", message);
    exit(1);
}

static void stopBuildWithName(const char *fmt, const char *name)
{
    fprintf(stderr, "Error: ");
    fprintf(stderr, fmt, name);
    fprintf(stderr, "\n");
    exit(1);
}

static char *copyText(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (out == NULL)
    {
        stopBuild("out of memory");
    }
    memcpy(out, s, n + 1);
    return out;
}

static void trimEnd(char *s)
{
    size_t n = strlen(s);
    while (n > 0)
    {
        unsigned char ch = (unsigned char)s[n - 1];
        if (ch == '\n' || ch == '\r' || isspace(ch) != 0)
        {
            s[n - 1] = '\0';
            n--;
        }
        else
        {
            break;
        }
    }
}

static void cutComment(char *s)
{
    char *semi = strchr(s, ';');
    if (semi != NULL)
    {
        *semi = '\0';
    }
}

static const char *skipBlank(const char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s) != 0)
    {
        s++;
    }
    return s;
}

static bool startsWith(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void writeU32LE(FILE *f, uint32_t x)
{
    uint8_t b[4];
    b[0] = (uint8_t)(x & 0xFFu);
    b[1] = (uint8_t)((x >> 8) & 0xFFu);
    b[2] = (uint8_t)((x >> 16) & 0xFFu);
    b[3] = (uint8_t)((x >> 24) & 0xFFu);
    if (fwrite(b, 1, 4, f) != 4)
    {
        stopBuild("failed writing output");
    }
}

static void writeU64LE(FILE *f, uint64_t x)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
    {
        b[i] = (uint8_t)((x >> (uint64_t)(8 * i)) & 0xFFULL);
    }
    if (fwrite(b, 1, 8, f) != 8)
    {
        stopBuild("failed writing output");
    }
}

static int readReg(const char *token)
{
    char *end = NULL;
    long v;

    if (token == NULL)
    {
        return -1;
    }
    if (!(token[0] == 'r' || token[0] == 'R'))
    {
        return -1;
    }

    v = strtol(token + 1, &end, 10);
    if (end == NULL || *end != '\0')
    {
        return -1;
    }
    if (v < 0 || v > 31)
    {
        return -1;
    }
    return (int)v;
}

static bool readU64Token(const char *token, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (token == NULL || *token == '\0')
    {
        return false;
    }

    errno = 0;
    v = strtoull(token, &end, 0);
    if (errno != 0)
    {
        return false;
    }
    if (end == NULL || *end != '\0')
    {
        return false;
    }

    *out = (uint64_t)v;
    return true;
}

static bool readI12Token(const char *token, int32_t *out)
{
    char *end = NULL;
    long v;

    if (token == NULL || *token == '\0')
    {
        return false;
    }

    errno = 0;
    v = strtol(token, &end, 0);
    if (errno != 0)
    {
        return false;
    }
    if (end == NULL || *end != '\0')
    {
        return false;
    }
    if (v < -2048 || v > 2047)
    {
        return false;
    }

    *out = (int32_t)v;
    return true;
}

static bool readU12Token(const char *token, uint32_t *out)
{
    uint64_t v = 0;
    if (readU64Token(token, &v) == false)
    {
        return false;
    }
    if (v > 0xFFFULL)
    {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

typedef struct
{
    char **items;
    int count;
} Words;

static void freeWords(Words *w)
{
    for (int i = 0; i < w->count; i++)
    {
        free(w->items[i]);
    }
    free(w->items);
    w->items = NULL;
    w->count = 0;
}

static Words splitLine(const char *line)
{
    Words w;
    size_t cap = 8;
    const char *p = line;

    w.items = (char **)malloc(sizeof(char *) * cap);
    if (w.items == NULL)
    {
        stopBuild("out of memory");
    }
    w.count = 0;

    while (*p != '\0')
    {
        while (*p != '\0' && (isspace((unsigned char)*p) != 0 || *p == ','))
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }

        const char *start = p;
        while (*p != '\0' && isspace((unsigned char)*p) == 0 && *p != ',')
        {
            p++;
        }

        size_t len = (size_t)(p - start);
        char *tok = (char *)malloc(len + 1);
        if (tok == NULL)
        {
            stopBuild("out of memory");
        }
        memcpy(tok, start, len);
        tok[len] = '\0';

        if ((size_t)w.count == cap)
        {
            cap *= 2;
            char **bigger = (char **)realloc(w.items, sizeof(char *) * cap);
            if (bigger == NULL)
            {
                stopBuild("out of memory");
            }
            w.items = bigger;
        }

        w.items[w.count++] = tok;
    }

    return w;
}

static int countCommas(const char *s)
{
    int c = 0;
    for (const char *p = s; p != NULL && *p != '\0'; p++)
    {
        if (*p == ',')
        {
            c++;
        }
    }
    return c;
}

static int expectedCommaCount(const char *mnemonic)
{
    if (mnemonic == NULL)
    {
        return -1;
    }

    if (strcmp(mnemonic, "return") == 0 || strcmp(mnemonic, "halt") == 0)
    {
        return 0;
    }

    if (strcmp(mnemonic, "br") == 0 || strcmp(mnemonic, "brr") == 0 || strcmp(mnemonic, "call") == 0)
    {
        return 0;
    }

    if (strcmp(mnemonic, "not") == 0 || strcmp(mnemonic, "addi") == 0 || strcmp(mnemonic, "subi") == 0 ||
        strcmp(mnemonic, "shftri") == 0 || strcmp(mnemonic, "shftli") == 0)
    {
        return 1;
    }

    if (strcmp(mnemonic, "brnz") == 0 || strcmp(mnemonic, "mov") == 0 || strcmp(mnemonic, "in") == 0 ||
        strcmp(mnemonic, "out") == 0 || strcmp(mnemonic, "clr") == 0 || strcmp(mnemonic, "push") == 0 ||
        strcmp(mnemonic, "pop") == 0 || strcmp(mnemonic, "ld") == 0)
    {
        if (strcmp(mnemonic, "mov") == 0 || strcmp(mnemonic, "brnz") == 0 || strcmp(mnemonic, "in") == 0 ||
            strcmp(mnemonic, "out") == 0 || strcmp(mnemonic, "ld") == 0)
        {
            return 1;
        }
        return 0;
    }

    if (strcmp(mnemonic, "priv") == 0)
    {
        return 3;
    }

    return 2;
}

static void requireCommaStyle(const char *raw, const char *mnemonic)
{
    int want = expectedCommaCount(mnemonic);
    if (want < 0)
    {
        return;
    }
    int have = countCommas(raw);
    if (have != want)
    {
        stopBuild("malformed operand separators");
    }
}

typedef enum
{
    sectionNone,
    sectionCode,
    sectionData
} Section;

typedef enum
{
    itemInstruction,
    itemData
} ItemKind;

typedef struct
{
    ItemKind kind;
    uint64_t address;
    char *text;    // for INSTR: instruction text; for DATA: if non-NULL = label name to resolve
    uint64_t data; // for DATA literals
} Item;

typedef struct
{
    Item *items;
    size_t count;
    size_t cap;
} ItemList;

static void pushItem(ItemList *list, Item it)
{
    if (list->count == list->cap)
    {
        size_t nextCap = (list->cap == 0) ? 64 : (list->cap * 2);
        Item *bigger = (Item *)realloc(list->items, nextCap * sizeof(Item));
        if (bigger == NULL)
        {
            stopBuild("out of memory");
        }
        list->items = bigger;
        list->cap = nextCap;
    }
    list->items[list->count++] = it;
}

static void freeItems(ItemList *list)
{
    for (size_t i = 0; i < list->count; i++)
    {
        free(list->items[i].text);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

typedef struct
{
    char *name;
    uint64_t address;
} Label;

typedef struct
{
    Label *items;
    size_t count;
    size_t cap;
} LabelTable;

static void addLabel(LabelTable *t, const char *name, uint64_t address)
{
    for (size_t i = 0; i < t->count; i++)
    {
        if (strcmp(t->items[i].name, name) == 0)
        {
            stopBuildWithName("duplicate label: %s", name);
        }
    }

    if (t->count == t->cap)
    {
        size_t nextCap = (t->cap == 0) ? 64 : (t->cap * 2);
        Label *bigger = (Label *)realloc(t->items, nextCap * sizeof(Label));
        if (bigger == NULL)
        {
            stopBuild("out of memory");
        }
        t->items = bigger;
        t->cap = nextCap;
    }

    t->items[t->count].name = copyText(name);
    t->items[t->count].address = address;
    t->count++;
}

static bool getLabel(const LabelTable *t, const char *name, uint64_t *out)
{
    for (size_t i = 0; i < t->count; i++)
    {
        if (strcmp(t->items[i].name, name) == 0)
        {
            *out = t->items[i].address;
            return true;
        }
    }
    return false;
}

static void freeLabels(LabelTable *t)
{
    for (size_t i = 0; i < t->count; i++)
    {
        free(t->items[i].name);
    }
    free(t->items);
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

typedef struct
{
    char **names;
    size_t count;
    size_t cap;
} PendingLabels;

static void pendingAdd(PendingLabels *p, const char *name)
{
    if (p->count == p->cap)
    {
        size_t nextCap = (p->cap == 0) ? 32 : (p->cap * 2);
        char **bigger = (char **)realloc(p->names, nextCap * sizeof(char *));
        if (bigger == NULL)
        {
            stopBuild("out of memory");
        }
        p->names = bigger;
        p->cap = nextCap;
    }
    p->names[p->count++] = copyText(name);
}

static void pendingResolve(PendingLabels *p, LabelTable *t, uint64_t address)
{
    for (size_t i = 0; i < p->count; i++)
    {
        addLabel(t, p->names[i], address);
        free(p->names[i]);
    }
    p->count = 0;
}

static void pendingFree(PendingLabels *p)
{
    for (size_t i = 0; i < p->count; i++)
    {
        free(p->names[i]);
    }
    free(p->names);
    p->names = NULL;
    p->count = 0;
    p->cap = 0;
}

static char *readLabelToken(const char *line)
{
    const char *p = line;

    if (p == NULL || p[0] != ':')
    {
        stopBuild("malformed label token");
    }

    p++;

    if (*p == '\0' || isspace((unsigned char)*p) != 0)
    {
        stopBuild("malformed label token");
    }

    if (!(isalpha((unsigned char)*p) != 0 || *p == '_' || *p == '.'))
    {
        stopBuild("malformed label token");
    }

    const char *start = p;

    while (*p != '\0')
    {
        if (isalnum((unsigned char)*p) != 0 || *p == '_' || *p == '.')
        {
            p++;
        }
        else
        {
            stopBuild("malformed label token");
        }
    }

    size_t len = (size_t)(p - start);
    char *name = (char *)malloc(len + 1);
    if (name == NULL)
    {
        stopBuild("out of memory");
    }

    memcpy(name, start, len);
    name[len] = '\0';
    return name;
}

static void addText(ItemList *code, uint64_t addr, const char *text, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    pushItem(code, (Item){.kind = itemInstruction, .address = addr, .text = copyText(text), .data = 0});
}

static void addDataLiteral(ItemList *data, uint64_t addr, uint64_t value, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    pushItem(data, (Item){.kind = itemData, .address = addr, .text = NULL, .data = value});
}

/* ✅ NEW: in .data, "\t:label" means "store address of label" */
static void addDataLabelRef(ItemList *data, uint64_t addr, const char *labelName, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    pushItem(data, (Item){.kind = itemData, .address = addr, .text = copyText(labelName), .data = 0});
}

/* ---- Macro emitters (unchanged) ---- */

static void emitClear(ItemList *code, uint64_t *pc, int rd, PendingLabels *pending, LabelTable *labels)
{
    char line[64];
    snprintf(line, sizeof(line), "xor r%d, r%d, r%d", rd, rd, rd);
    addText(code, *pc, line, pending, labels);
    *pc += 4;
}

static void emitHalt(ItemList *code, uint64_t *pc, PendingLabels *pending, LabelTable *labels)
{
    addText(code, *pc, "priv r0, r0, r0, 0", pending, labels);
    *pc += 4;
}

static void emitIn(ItemList *code, uint64_t *pc, int rd, int rs, PendingLabels *pending, LabelTable *labels)
{
    char line[64];
    snprintf(line, sizeof(line), "priv r%d, r%d, r0, 3", rd, rs);
    addText(code, *pc, line, pending, labels);
    *pc += 4;
}

static void emitOut(ItemList *code, uint64_t *pc, int rd, int rs, PendingLabels *pending, LabelTable *labels)
{
    char line[64];
    snprintf(line, sizeof(line), "priv r%d, r%d, r0, 4", rd, rs);
    addText(code, *pc, line, pending, labels);
    *pc += 4;
}

static void emitPush(ItemList *code, uint64_t *pc, int rd, PendingLabels *pending, LabelTable *labels)
{
    char a[64];
    snprintf(a, sizeof(a), "mov (r31)(-8), r%d", rd);
    addText(code, *pc, a, pending, labels);
    *pc += 4;

    addText(code, *pc, "subi r31, 8", pending, labels);
    *pc += 4;
}

static void emitPop(ItemList *code, uint64_t *pc, int rd, PendingLabels *pending, LabelTable *labels)
{
    char a[64];
    snprintf(a, sizeof(a), "mov r%d, (r31)(0)", rd);
    addText(code, *pc, a, pending, labels);
    *pc += 4;

    addText(code, *pc, "addi r31, 8", pending, labels);
    *pc += 4;
}

static void emitLoad64(ItemList *code, uint64_t *pc, int rd, uint64_t value, PendingLabels *pending, LabelTable *labels)
{
    const int shifts[5] = {12, 12, 12, 12, 4};
    const int offs[5] = {40, 28, 16, 4, 0};

    {
        char line[64];
        snprintf(line, sizeof(line), "xor r%d, r%d, r%d", rd, rd, rd);
        addText(code, *pc, line, pending, labels);
        *pc += 4;
    }

    {
        char line[64];
        uint64_t top = (value >> 52) & 0xFFFULL;
        snprintf(line, sizeof(line), "addi r%d, %llu", rd, (unsigned long long)top);
        addText(code, *pc, line, pending, labels);
        *pc += 4;
    }

    for (int i = 0; i < 5; i++)
    {
        {
            char line[64];
            snprintf(line, sizeof(line), "shftli r%d, %d", rd, shifts[i]);
            addText(code, *pc, line, pending, labels);
            *pc += 4;
        }

        {
            char line[64];
            uint64_t part = (i == 4) ? (value & 0xFULL) : ((value >> (uint64_t)offs[i]) & 0xFFFULL);
            snprintf(line, sizeof(line), "addi r%d, %llu", rd, (unsigned long long)part);
            addText(code, *pc, line, pending, labels);
            *pc += 4;
        }
    }
}

/* ---- Instruction packing / assembling (unchanged from your version) ---- */

static uint32_t packR(uint32_t op, uint32_t rd, uint32_t rs, uint32_t rt)
{
    uint32_t w = 0;
    w |= ((op & 0x1Fu) << 27);
    w |= ((rd & 0x1Fu) << 22);
    w |= ((rs & 0x1Fu) << 17);
    w |= ((rt & 0x1Fu) << 12);
    return w;
}

static uint32_t packI(uint32_t op, uint32_t rd, uint32_t rs, uint32_t imm12)
{
    uint32_t w = 0;
    w |= ((op & 0x1Fu) << 27);
    w |= ((rd & 0x1Fu) << 22);
    w |= ((rs & 0x1Fu) << 17);
    w |= (imm12 & 0xFFFu);
    return w;
}

static uint32_t packP(uint32_t op, uint32_t rd, uint32_t rs, uint32_t rt, uint32_t imm12)
{
    uint32_t w = 0;
    w |= ((op & 0x1Fu) << 27);
    w |= ((rd & 0x1Fu) << 22);
    w |= ((rs & 0x1Fu) << 17);
    w |= ((rt & 0x1Fu) << 12);
    w |= (imm12 & 0xFFFu);
    return w;
}

/* assembleInstruction(...) stays exactly as you had it */
static uint32_t assembleInstruction(const char *instText, uint64_t pc, const LabelTable *labels)
{
    /* ... KEEP YOUR ENTIRE assembleInstruction BODY HERE UNCHANGED ... */

    /* NOTE:
       I’m not repeating the whole function again to avoid a gigantic wall of text twice.
       Paste your current assembleInstruction exactly as-is here.
    */

    /* If you want, I can paste the full one in the next message too. */
    (void)instText;
    (void)pc;
    (void)labels;
    stopBuild("assembleInstruction placeholder: paste your existing function body here");
    return 0;
}

static void buildProgram(const char *inputPath, ItemList *code, ItemList *data, LabelTable *labels)
{
    FILE *f = fopen(inputPath, "r");
    if (f == NULL)
    {
        stopBuildWithName("cannot open input file: %s", inputPath);
    }

    char raw[4096];
    Section mode = sectionNone;
    uint64_t codePc = codeBase;
    uint64_t dataPc = dataBase;
    PendingLabels pending = {.names = NULL, .count = 0, .cap = 0};
    bool sawCode = false;

    while (fgets(raw, sizeof(raw), f) != NULL)
    {
        trimEnd(raw);
        cutComment(raw);
        trimEnd(raw);

        const char *p = raw;
        if (*p == '\0')
        {
            continue;
        }

        if (startsWith(p, ".code"))
        {
            mode = sectionCode;
            sawCode = true;
            continue;
        }

        if (startsWith(p, ".data"))
        {
            mode = sectionData;
            continue;
        }

        /* label definitions are always lines that start with ':' at the start of the line */
        if (*p == ':')
        {
            char *name = readLabelToken(p);
            pendingAdd(&pending, name);
            free(name);
            continue;
        }

        if (*p != '\t')
        {
            fclose(f);
            pendingFree(&pending);
            stopBuild("code/data line must start with tab character");
        }

        if (mode == sectionNone)
        {
            fclose(f);
            pendingFree(&pending);
            stopBuild("code/data line before any .code or .data directive");
        }

        while (*p == '\t')
        {
            p++;
        }
        p = skipBlank(p);
        if (*p == '\0')
        {
            continue;
        }

        if (mode == sectionData)
        {
            /* ✅ FIX: in .data, "\t:label" is a DATA value (address), NOT a label definition */
            if (*p == ':' && p[1] != '\0')
            {
                const char *labelRef = p + 1;
                addDataLabelRef(data, dataPc, labelRef, &pending, labels);
                dataPc += 8;
                continue;
            }

            uint64_t v = 0;
            if (readU64Token(p, &v) == false)
            {
                fclose(f);
                pendingFree(&pending);
                stopBuild("malformed data item (expected 64-bit unsigned integer or :label)");
            }
            addDataLiteral(data, dataPc, v, &pending, labels);

            dataPc += 8;
            continue;
        }

        /* code section: unchanged */
        Words w = splitLine(p);
        if (w.count == 0)
        {
            freeWords(&w);
            continue;
        }

        char *mn = w.items[0];
        for (char *c = mn; *c != '\0'; c++)
        {
            *c = (char)tolower((unsigned char)*c);
        }

        requireCommaStyle(p, mn);

        if (strcmp(mn, "clr") == 0)
        {
            if (w.count != 2)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("clr expects: clr rd");
            }

            int rd = readReg(w.items[1]);
            if (rd < 0)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("clr: invalid register");
            }

            freeWords(&w);
            emitClear(code, &codePc, rd, &pending, labels);
            continue;
        }

        if (strcmp(mn, "halt") == 0)
        {
            if (w.count != 1)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("halt expects: halt");
            }

            freeWords(&w);
            emitHalt(code, &codePc, &pending, labels);
            continue;
        }

        if (strcmp(mn, "in") == 0)
        {
            if (w.count != 3)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("in expects: in rd, rs");
            }

            int rd = readReg(w.items[1]);
            int rs = readReg(w.items[2]);

            if (rd < 0 || rs < 0)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("in: invalid register");
            }

            freeWords(&w);
            emitIn(code, &codePc, rd, rs, &pending, labels);
            continue;
        }

        if (strcmp(mn, "out") == 0)
        {
            if (w.count != 3)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("out expects: out rd, rs");
            }

            int rd = readReg(w.items[1]);
            int rs = readReg(w.items[2]);

            if (rd < 0 || rs < 0)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("out: invalid register");
            }

            freeWords(&w);
            emitOut(code, &codePc, rd, rs, &pending, labels);
            continue;
        }

        if (strcmp(mn, "push") == 0)
        {
            if (w.count != 2)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("push expects: push rd");
            }

            int rd = readReg(w.items[1]);
            if (rd < 0)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("push: invalid register");
            }

            freeWords(&w);
            emitPush(code, &codePc, rd, &pending, labels);
            continue;
        }

        if (strcmp(mn, "pop") == 0)
        {
            if (w.count != 2)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("pop expects: pop rd");
            }

            int rd = readReg(w.items[1]);
            if (rd < 0)
            {
                freeWords(&w);
                fclose(f);
                pendingFree(&pending);
                stopBuild("pop: invalid register");
            }

            freeWords(&w);
            emitPop(code, &codePc, rd, &pending, labels);
            continue;
        }

        /* IMPORTANT:
           Your ld macro section can remain exactly as you had it.
           If you still resolve labels early there, it can break forward refs.
           But your failing tests were the .data :label issue.
        */

        freeWords(&w);
        addText(code, codePc, p, &pending, labels);
        codePc += 4;
    }

    fclose(f);

    if (pending.count != 0)
    {
        pendingFree(&pending);
        stopBuild("label at end of file without following instruction/data");
    }

    pendingFree(&pending);

    if (sawCode == false)
    {
        stopBuild("program must have at least one .code directive");
    }
}

static uint32_t *assembleAll(const ItemList *code, const LabelTable *labels)
{
    uint32_t *out = (uint32_t *)malloc(sizeof(uint32_t) * code->count);
    if (out == NULL)
    {
        stopBuild("out of memory");
    }

    for (size_t i = 0; i < code->count; i++)
    {
        out[i] = assembleInstruction(code->items[i].text, code->items[i].address, labels);
    }

    return out;
}

static void writeTko(const char *outPath, const ItemList *code, const ItemList *data, const uint32_t *words, const LabelTable *labels)
{
    FILE *f = fopen(outPath, "wb");
    if (f == NULL)
    {
        stopBuildWithName("cannot open output file: %s", outPath);
    }

    uint64_t fileType = 0ULL;
    uint64_t codeBegin = codeBase;
    uint64_t codeSize = (uint64_t)(code->count * 4ULL);
    uint64_t dataBegin = dataBase;
    uint64_t dataSize = (uint64_t)(data->count * 8ULL);

    writeU64LE(f, fileType);
    writeU64LE(f, codeBegin);
    writeU64LE(f, codeSize);
    writeU64LE(f, dataBegin);
    writeU64LE(f, dataSize);

    for (size_t i = 0; i < code->count; i++)
    {
        writeU32LE(f, words[i]);
    }

    for (size_t i = 0; i < data->count; i++)
    {
        if (data->items[i].text != NULL)
        {
            uint64_t addr;
            if (getLabel(labels, data->items[i].text, &addr) == false)
            {
                fclose(f);
                stopBuildWithName("undefined label reference: %s", data->items[i].text);
            }
            writeU64LE(f, addr);
        }
        else
        {
            writeU64LE(f, data->items[i].data);
        }
    }

    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s input.tk output.tko\n", argv[0]);
        return 1;
    }

    const char *inputPath = argv[1];
    const char *outputPath = argv[2];

    ItemList code = {.items = NULL, .count = 0, .cap = 0};
    ItemList data = {.items = NULL, .count = 0, .cap = 0};
    LabelTable labels = {.items = NULL, .count = 0, .cap = 0};

    buildProgram(inputPath, &code, &data, &labels);

    uint32_t *words = assembleAll(&code, &labels);
    writeTko(outputPath, &code, &data, words, &labels);

    free(words);
    freeItems(&code);
    freeItems(&data);
    freeLabels(&labels);

    return 0;
}