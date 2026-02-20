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
    size_t n;
    char *out;

    n = strlen(s);
    out = (char *)malloc(n + 1);
    if (out == NULL)
    {
        stopBuild("out of memory");
    }

    memcpy(out, s, n + 1);
    return out;
}

static void trimEnd(char *s)
{
    size_t n;

    n = strlen(s);
    while (n > 0)
    {
        unsigned char ch;
        ch = (unsigned char)s[n - 1];

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
    char *semi;

    semi = strchr(s, ';');
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
    size_t got;

    b[0] = (uint8_t)(x & 0xFFu);
    b[1] = (uint8_t)((x >> 8) & 0xFFu);
    b[2] = (uint8_t)((x >> 16) & 0xFFu);
    b[3] = (uint8_t)((x >> 24) & 0xFFu);

    got = fwrite(b, 1, 4, f);
    if (got != 4)
    {
        stopBuild("failed writing output");
    }
}

static void writeU64LE(FILE *f, uint64_t x)
{
    uint8_t b[8];
    size_t got;
    int i;

    i = 0;
    while (i < 8)
    {
        b[i] = (uint8_t)((x >> (uint64_t)(8 * i)) & 0xFFULL);
        i++;
    }

    got = fwrite(b, 1, 8, f);
    if (got != 8)
    {
        stopBuild("failed writing output");
    }
}

static int readReg(const char *token)
{
    char *end;
    long v;

    if (token == NULL)
    {
        return -1;
    }

    if (!(token[0] == 'r' || token[0] == 'R'))
    {
        return -1;
    }

    end = NULL;
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
    char *end;
    unsigned long long v;

    if (token == NULL || *token == '\0')
    {
        return false;
    }

    errno = 0;
    end = NULL;
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
    char *end;
    long v;

    if (token == NULL || *token == '\0')
    {
        return false;
    }

    errno = 0;
    end = NULL;
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
    uint64_t v;

    v = 0;
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
    int i;

    i = 0;
    while (i < w->count)
    {
        free(w->items[i]);
        i++;
    }

    free(w->items);
    w->items = NULL;
    w->count = 0;
}

static Words splitLine(const char *line)
{
    Words w;
    size_t cap;
    const char *p;

    w.items = NULL;
    w.count = 0;

    cap = 8;
    w.items = (char **)malloc(sizeof(char *) * cap);
    if (w.items == NULL)
    {
        stopBuild("out of memory");
    }

    p = line;

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

        const char *start;
        size_t len;
        char *tok;

        start = p;

        while (*p != '\0' && isspace((unsigned char)*p) == 0 && *p != ',')
        {
            p++;
        }

        len = (size_t)(p - start);
        tok = (char *)malloc(len + 1);
        if (tok == NULL)
        {
            stopBuild("out of memory");
        }

        memcpy(tok, start, len);
        tok[len] = '\0';

        if ((size_t)w.count == cap)
        {
            char **bigger;

            cap = cap * 2;
            bigger = (char **)realloc(w.items, sizeof(char *) * cap);
            if (bigger == NULL)
            {
                stopBuild("out of memory");
            }
            w.items = bigger;
        }

        w.items[w.count] = tok;
        w.count = w.count + 1;
    }

    return w;
}

static int countCommas(const char *s)
{
    int c;
    const char *p;

    c = 0;
    p = s;

    while (p != NULL && *p != '\0')
    {
        if (*p == ',')
        {
            c++;
        }
        p++;
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
    int want;
    int have;

    want = expectedCommaCount(mnemonic);
    if (want < 0)
    {
        return;
    }

    have = countCommas(raw);
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
    char *text;
    uint64_t data;
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
        size_t nextCap;
        Item *bigger;

        nextCap = list->cap;

        if (nextCap == 0)
        {
            nextCap = 64;
        }
        else
        {
            nextCap = nextCap * 2;
        }

        bigger = (Item *)realloc(list->items, nextCap * sizeof(Item));
        if (bigger == NULL)
        {
            stopBuild("out of memory");
        }

        list->items = bigger;
        list->cap = nextCap;
    }

    list->items[list->count] = it;
    list->count = list->count + 1;
}

static void freeItems(ItemList *list)
{
    size_t i;

    i = 0;
    while (i < list->count)
    {
        free(list->items[i].text);
        i++;
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
    size_t i;

    i = 0;
    while (i < t->count)
    {
        if (strcmp(t->items[i].name, name) == 0)
        {
            stopBuildWithName("duplicate label: %s", name);
        }
        i++;
    }

    if (t->count == t->cap)
    {
        size_t nextCap;
        Label *bigger;

        nextCap = t->cap;

        if (nextCap == 0)
        {
            nextCap = 64;
        }
        else
        {
            nextCap = nextCap * 2;
        }

        bigger = (Label *)realloc(t->items, nextCap * sizeof(Label));
        if (bigger == NULL)
        {
            stopBuild("out of memory");
        }

        t->items = bigger;
        t->cap = nextCap;
    }

    t->items[t->count].name = copyText(name);
    t->items[t->count].address = address;
    t->count = t->count + 1;
}

static bool getLabel(const LabelTable *t, const char *name, uint64_t *out)
{
    size_t i;

    i = 0;
    while (i < t->count)
    {
        if (strcmp(t->items[i].name, name) == 0)
        {
            *out = t->items[i].address;
            return true;
        }
        i++;
    }

    return false;
}

static void freeLabels(LabelTable *t)
{
    size_t i;

    i = 0;
    while (i < t->count)
    {
        free(t->items[i].name);
        i++;
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
        size_t nextCap;
        char **bigger;

        nextCap = p->cap;

        if (nextCap == 0)
        {
            nextCap = 32;
        }
        else
        {
            nextCap = nextCap * 2;
        }

        bigger = (char **)realloc(p->names, nextCap * sizeof(char *));
        if (bigger == NULL)
        {
            stopBuild("out of memory");
        }

        p->names = bigger;
        p->cap = nextCap;
    }

    p->names[p->count] = copyText(name);
    p->count = p->count + 1;
}

static void pendingResolve(PendingLabels *p, LabelTable *t, uint64_t address)
{
    size_t i;

    i = 0;
    while (i < p->count)
    {
        addLabel(t, p->names[i], address);
        free(p->names[i]);
        i++;
    }

    p->count = 0;
}

static void pendingFree(PendingLabels *p)
{
    size_t i;

    i = 0;
    while (i < p->count)
    {
        free(p->names[i]);
        i++;
    }

    free(p->names);
    p->names = NULL;
    p->count = 0;
    p->cap = 0;
}

static char *readLabelToken(const char *line)
{
    const char *p;
    const char *start;
    size_t len;
    char *name;

    if (line == NULL || line[0] != ':')
    {
        stopBuild("malformed label token");
    }

    p = line + 1;

    if (*p == '\0' || isspace((unsigned char)*p) != 0)
    {
        stopBuild("malformed label token");
    }

    if (!(isalpha((unsigned char)*p) != 0 || *p == '_' || *p == '.'))
    {
        stopBuild("malformed label token");
    }

    start = p;

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

    len = (size_t)(p - start);
    name = (char *)malloc(len + 1);
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

static void addData(ItemList *data, uint64_t addr, uint64_t value, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    pushItem(data, (Item){.kind = itemData, .address = addr, .text = NULL, .data = value});
}

static void emitClear(ItemList *code, uint64_t *pc, int rd, PendingLabels *pending, LabelTable *labels)
{
    char line[64];

    snprintf(line, sizeof(line), "xor r%d, r%d, r%d", rd, rd, rd);
    addText(code, *pc, line, pending, labels);
    *pc = *pc + 4;
}

static void emitHalt(ItemList *code, uint64_t *pc, PendingLabels *pending, LabelTable *labels)
{
    addText(code, *pc, "priv r0, r0, r0, 0", pending, labels);
    *pc = *pc + 4;
}

static void emitIn(ItemList *code, uint64_t *pc, int rd, int rs, PendingLabels *pending, LabelTable *labels)
{
    char line[64];

    snprintf(line, sizeof(line), "priv r%d, r%d, r0, 3", rd, rs);
    addText(code, *pc, line, pending, labels);
    *pc = *pc + 4;
}

static void emitOut(ItemList *code, uint64_t *pc, int rd, int rs, PendingLabels *pending, LabelTable *labels)
{
    char line[64];

    snprintf(line, sizeof(line), "priv r%d, r%d, r0, 4", rd, rs);
    addText(code, *pc, line, pending, labels);
    *pc = *pc + 4;
}

static void emitPush(ItemList *code, uint64_t *pc, int rd, PendingLabels *pending, LabelTable *labels)
{
    char a[64];

    snprintf(a, sizeof(a), "mov (r31)(-8), r%d", rd);
    addText(code, *pc, a, pending, labels);
    *pc = *pc + 4;

    addText(code, *pc, "subi r31, 8", pending, labels);
    *pc = *pc + 4;
}

static void emitPop(ItemList *code, uint64_t *pc, int rd, PendingLabels *pending, LabelTable *labels)
{
    char a[64];

    snprintf(a, sizeof(a), "mov r%d, (r31)(0)", rd);
    addText(code, *pc, a, pending, labels);
    *pc = *pc + 4;

    addText(code, *pc, "addi r31, 8", pending, labels);
    *pc = *pc + 4;
}

static void emitLoad64(ItemList *code, uint64_t *pc, int rd, uint64_t value, PendingLabels *pending, LabelTable *labels)
{
    const int shifts[5] = {12, 12, 12, 12, 4};
    const int offs[5] = {40, 28, 16, 4, 0};

    {
        char line[64];
        snprintf(line, sizeof(line), "xor r%d, r%d, r%d", rd, rd, rd);
        addText(code, *pc, line, pending, labels);
        *pc = *pc + 4;
    }

    {
        char line[64];
        uint64_t top;
        top = (value >> 52) & 0xFFFULL;
        snprintf(line, sizeof(line), "addi r%d, %llu", rd, (unsigned long long)top);
        addText(code, *pc, line, pending, labels);
        *pc = *pc + 4;
    }

    int i;

    i = 0;
    while (i < 5)
    {
        {
            char line[64];
            snprintf(line, sizeof(line), "shftli r%d, %d", rd, shifts[i]);
            addText(code, *pc, line, pending, labels);
            *pc = *pc + 4;
        }

        {
            char line[64];
            uint64_t part;

            if (i == 4)
            {
                part = value & 0xFULL;
            }
            else
            {
                part = (value >> (uint64_t)offs[i]) & 0xFFFULL;
            }

            snprintf(line, sizeof(line), "addi r%d, %llu", rd, (unsigned long long)part);
            addText(code, *pc, line, pending, labels);
            *pc = *pc + 4;
        }

        i++;
    }
}

static uint32_t packR(uint32_t op, uint32_t rd, uint32_t rs, uint32_t rt)
{
    uint32_t w;
    w = 0;
    w |= ((op & 0x1Fu) << 27);
    w |= ((rd & 0x1Fu) << 22);
    w |= ((rs & 0x1Fu) << 17);
    w |= ((rt & 0x1Fu) << 12);
    return w;
}

static uint32_t packI(uint32_t op, uint32_t rd, uint32_t rs, uint32_t imm12)
{
    uint32_t w;
    w = 0;
    w |= ((op & 0x1Fu) << 27);
    w |= ((rd & 0x1Fu) << 22);
    w |= ((rs & 0x1Fu) << 17);
    w |= (imm12 & 0xFFFu);
    return w;
}

static uint32_t packP(uint32_t op, uint32_t rd, uint32_t rs, uint32_t rt, uint32_t imm12)
{
    uint32_t w;
    w = 0;
    w |= ((op & 0x1Fu) << 27);
    w |= ((rd & 0x1Fu) << 22);
    w |= ((rs & 0x1Fu) << 17);
    w |= ((rt & 0x1Fu) << 12);
    w |= (imm12 & 0xFFFu);
    return w;
}

static uint32_t assembleInstruction(const char *instText, uint64_t pc, const LabelTable *labels)
{
    Words w;
    const char *mn;

    w = splitLine(instText);
    if (w.count == 0)
    {
        freeWords(&w);
        stopBuild("empty instruction");
    }

    {
        char *c;
        c = w.items[0];
        while (*c != '\0')
        {
            *c = (char)tolower((unsigned char)*c);
            c++;
        }
    }

    mn = w.items[0];

    if (strcmp(mn, "and") == 0 || strcmp(mn, "or") == 0 || strcmp(mn, "xor") == 0 ||
        strcmp(mn, "add") == 0 || strcmp(mn, "sub") == 0 || strcmp(mn, "mul") == 0 || strcmp(mn, "div") == 0 ||
        strcmp(mn, "addf") == 0 || strcmp(mn, "subf") == 0 || strcmp(mn, "mulf") == 0 || strcmp(mn, "divf") == 0 ||
        strcmp(mn, "shftr") == 0 || strcmp(mn, "shftl") == 0)
    {
        int rd;
        int rs;
        int rt;
        uint32_t op;

        if (w.count != 4)
        {
            freeWords(&w);
            stopBuild("R-type expects 3 registers");
        }

        rd = readReg(w.items[1]);
        rs = readReg(w.items[2]);
        rt = readReg(w.items[3]);

        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        op = 0;

        if (strcmp(mn, "and") == 0)
        {
            op = 0x00;
        }
        else if (strcmp(mn, "or") == 0)
        {
            op = 0x01;
        }
        else if (strcmp(mn, "xor") == 0)
        {
            op = 0x02;
        }
        else if (strcmp(mn, "shftr") == 0)
        {
            op = 0x04;
        }
        else if (strcmp(mn, "shftl") == 0)
        {
            op = 0x06;
        }
        else if (strcmp(mn, "addf") == 0)
        {
            op = 0x14;
        }
        else if (strcmp(mn, "subf") == 0)
        {
            op = 0x15;
        }
        else if (strcmp(mn, "mulf") == 0)
        {
            op = 0x16;
        }
        else if (strcmp(mn, "divf") == 0)
        {
            op = 0x17;
        }
        else if (strcmp(mn, "add") == 0)
        {
            op = 0x18;
        }
        else if (strcmp(mn, "sub") == 0)
        {
            op = 0x1A;
        }
        else if (strcmp(mn, "mul") == 0)
        {
            op = 0x1C;
        }
        else if (strcmp(mn, "div") == 0)
        {
            op = 0x1D;
        }
        else
        {
            freeWords(&w);
            stopBuild("unknown instruction");
        }

        freeWords(&w);
        return packR(op, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt);
    }

    if (strcmp(mn, "not") == 0)
    {
        int rd;
        int rs;

        if (w.count != 3)
        {
            freeWords(&w);
            stopBuild("not expects 2 registers");
        }

        rd = readReg(w.items[1]);
        rs = readReg(w.items[2]);

        if (rd < 0 || rs < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        freeWords(&w);
        return packR(0x03, (uint32_t)rd, (uint32_t)rs, 0);
    }

    if (strcmp(mn, "addi") == 0 || strcmp(mn, "subi") == 0 || strcmp(mn, "shftri") == 0 || strcmp(mn, "shftli") == 0)
    {
        int rd;
        uint32_t imm;
        uint32_t op;

        if (w.count != 3)
        {
            freeWords(&w);
            stopBuild("I-type expects rd, imm");
        }

        rd = readReg(w.items[1]);
        if (rd < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        if (readU12Token(w.items[2], &imm) == false)
        {
            freeWords(&w);
            stopBuild("immediate must be 0..4095");
        }

        op = 0;

        if (strcmp(mn, "addi") == 0)
        {
            op = 0x19;
        }
        else if (strcmp(mn, "subi") == 0)
        {
            op = 0x1B;
        }
        else if (strcmp(mn, "shftri") == 0)
        {
            op = 0x05;
        }
        else
        {
            op = 0x07;
        }

        freeWords(&w);
        return packI(op, (uint32_t)rd, 0, imm);
    }

    if (strcmp(mn, "br") == 0)
    {
        int rd;

        if (w.count != 2)
        {
            freeWords(&w);
            stopBuild("br expects rd");
        }

        rd = readReg(w.items[1]);
        if (rd < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        freeWords(&w);
        return packR(0x08, (uint32_t)rd, 0, 0);
    }

    if (strcmp(mn, "brr") == 0)
    {
        int r;
        int32_t rel;
        uint32_t imm12;
        uint64_t target;

        if (w.count != 2)
        {
            freeWords(&w);
            stopBuild("brr expects rd or imm");
        }

        r = readReg(w.items[1]);
        if (r >= 0)
        {
            freeWords(&w);
            return packR(0x09, (uint32_t)r, 0, 0);
        }

        if (w.items[1][0] == ':' && w.items[1][1] != '\0')
        {
            if (getLabel(labels, w.items[1] + 1, &target) == false)
            {
                freeWords(&w);
                stopBuildWithName("undefined label reference: %s", w.items[1] + 1);
            }

            {
                int64_t delta;
                delta = (int64_t)target - (int64_t)(pc + 4);
                if (delta < -2048 || delta > 2047)
                {
                    freeWords(&w);
                    stopBuild("brr label out of range for signed 12-bit");
                }
                rel = (int32_t)delta;
            }

            imm12 = (uint32_t)rel & 0xFFFu;
            freeWords(&w);
            return ((0x0Au & 0x1Fu) << 27) | imm12;
        }

        if (readI12Token(w.items[1], &rel) == false)
        {
            freeWords(&w);
            stopBuild("brr immediate must fit signed 12-bit");
        }

        imm12 = (uint32_t)rel & 0xFFFu;
        freeWords(&w);
        return ((0x0Au & 0x1Fu) << 27) | imm12;
    }

    if (strcmp(mn, "brnz") == 0)
    {
        int rd;
        int rs;

        if (w.count != 3)
        {
            freeWords(&w);
            stopBuild("brnz expects rd, rs");
        }

        rd = readReg(w.items[1]);
        rs = readReg(w.items[2]);

        if (rd < 0 || rs < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        freeWords(&w);
        return packR(0x0B, (uint32_t)rd, (uint32_t)rs, 0);
    }

    if (strcmp(mn, "call") == 0)
    {
        int rd;

        if (w.count != 2)
        {
            freeWords(&w);
            stopBuild("call expects rd");
        }

        rd = readReg(w.items[1]);
        if (rd < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        freeWords(&w);
        return packR(0x0C, (uint32_t)rd, 0, 0);
    }

    if (strcmp(mn, "return") == 0)
    {
        if (w.count != 1)
        {
            freeWords(&w);
            stopBuild("return expects no operands");
        }

        freeWords(&w);
        return ((0x0Du & 0x1Fu) << 27);
    }

    if (strcmp(mn, "brgt") == 0)
    {
        int rd;
        int rs;
        int rt;

        if (w.count != 4)
        {
            freeWords(&w);
            stopBuild("brgt expects rd, rs, rt");
        }

        rd = readReg(w.items[1]);
        rs = readReg(w.items[2]);
        rt = readReg(w.items[3]);

        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        freeWords(&w);
        return packR(0x0E, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt);
    }

    if (strcmp(mn, "priv") == 0)
    {
        int rd;
        int rs;
        int rt;
        uint32_t imm;

        if (w.count != 5)
        {
            freeWords(&w);
            stopBuild("priv expects rd, rs, rt, imm");
        }

        rd = readReg(w.items[1]);
        rs = readReg(w.items[2]);
        rt = readReg(w.items[3]);

        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeWords(&w);
            stopBuild("invalid register");
        }

        if (readU12Token(w.items[4], &imm) == false)
        {
            freeWords(&w);
            stopBuild("priv imm must be 0..4095");
        }

        freeWords(&w);
        return packP(0x0F, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt, imm);
    }

    if (strcmp(mn, "mov") == 0)
    {
        const char *left;
        const char *right;

        if (w.count != 3)
        {
            freeWords(&w);
            stopBuild("mov expects 2 operands");
        }

        left = w.items[1];
        right = w.items[2];

        if (left[0] == '(')
        {
            const char *p;
            char baseBuf[16];
            char immBuf[32];
            int bi;
            int ii;
            int base;
            int32_t imm;
            int src;

            p = left + 1;

            bi = 0;
            while (*p != '\0' && *p != ')' && bi < 15)
            {
                baseBuf[bi] = *p;
                bi++;
                p++;
            }
            baseBuf[bi] = '\0';

            if (*p != ')')
            {
                freeWords(&w);
                stopBuild("mov store: malformed operand");
            }
            p++;

            if (*p != '(')
            {
                freeWords(&w);
                stopBuild("mov store: expected second ()");
            }
            p++;

            ii = 0;
            while (*p != '\0' && *p != ')' && ii < 31)
            {
                immBuf[ii] = *p;
                ii++;
                p++;
            }
            immBuf[ii] = '\0';

            if (*p != ')')
            {
                freeWords(&w);
                stopBuild("mov store: malformed imm");
            }
            p++;

            if (*p != '\0')
            {
                freeWords(&w);
                stopBuild("mov store: trailing junk");
            }

            base = readReg(baseBuf);
            if (base < 0)
            {
                freeWords(&w);
                stopBuild("mov store: invalid base reg");
            }

            if (readI12Token(immBuf, &imm) == false)
            {
                freeWords(&w);
                stopBuild("mov store: imm must fit signed 12-bit");
            }

            src = readReg(right);
            if (src < 0)
            {
                freeWords(&w);
                stopBuild("mov store: invalid source reg");
            }

            freeWords(&w);
            return packP(0x13, (uint32_t)base, (uint32_t)src, 0, (uint32_t)imm & 0xFFFu);
        }

        if (right[0] == '(')
        {
            const char *p;
            char baseBuf[16];
            char immBuf[32];
            int bi;
            int ii;
            int base;
            int dst;
            int32_t imm;

            dst = readReg(left);
            if (dst < 0)
            {
                freeWords(&w);
                stopBuild("mov load: invalid rd");
            }

            p = right + 1;

            bi = 0;
            while (*p != '\0' && *p != ')' && bi < 15)
            {
                baseBuf[bi] = *p;
                bi++;
                p++;
            }
            baseBuf[bi] = '\0';

            if (*p != ')')
            {
                freeWords(&w);
                stopBuild("mov load: malformed operand");
            }
            p++;

            if (*p != '(')
            {
                freeWords(&w);
                stopBuild("mov load: expected second ()");
            }
            p++;

            ii = 0;
            while (*p != '\0' && *p != ')' && ii < 31)
            {
                immBuf[ii] = *p;
                ii++;
                p++;
            }
            immBuf[ii] = '\0';

            if (*p != ')')
            {
                freeWords(&w);
                stopBuild("mov load: malformed imm");
            }
            p++;

            if (*p != '\0')
            {
                freeWords(&w);
                stopBuild("mov load: trailing junk");
            }

            base = readReg(baseBuf);
            if (base < 0)
            {
                freeWords(&w);
                stopBuild("mov load: invalid base reg");
            }

            if (readI12Token(immBuf, &imm) == false)
            {
                freeWords(&w);
                stopBuild("mov load: imm must fit signed 12-bit");
            }

            freeWords(&w);
            return packP(0x10, (uint32_t)dst, (uint32_t)base, 0, (uint32_t)imm & 0xFFFu);
        }

        {
            int dst;
            int src;
            uint32_t imm;

            dst = readReg(left);
            if (dst < 0)
            {
                freeWords(&w);
                stopBuild("mov: invalid rd");
            }

            src = readReg(right);
            if (src >= 0)
            {
                freeWords(&w);
                return packR(0x11, (uint32_t)dst, (uint32_t)src, 0);
            }

            if (readU12Token(right, &imm) == false)
            {
                freeWords(&w);
                stopBuild("mov rd, L: L must be 0..4095");
            }

            freeWords(&w);
            return packI(0x12, (uint32_t)dst, 0, imm);
        }
    }

    freeWords(&w);
    stopBuildWithName("unknown instruction mnemonic: %s", mn);
    return 0;
}

static void buildProgram(const char *inputPath, ItemList *code, ItemList *data, LabelTable *labels)
{
    FILE *f;
    char raw[4096];
    Section mode;
    uint64_t codePc;
    uint64_t dataPc;
    PendingLabels pending;
    bool sawCode;

    f = fopen(inputPath, "r");
    if (f == NULL)
    {
        stopBuildWithName("cannot open input file: %s", inputPath);
    }

    mode = sectionNone;
    codePc = codeBase;
    dataPc = dataBase;
    pending.names = NULL;
    pending.count = 0;
    pending.cap = 0;
    sawCode = false;

    while (fgets(raw, sizeof(raw), f) != NULL)
    {
        trimEnd(raw);
        cutComment(raw);
        trimEnd(raw);

        const char *line;
        line = raw;

        line = skipBlank(line);
        if (*line == '\0')
        {
            continue;
        }

        if (startsWith(line, ".code"))
        {
            mode = sectionCode;
            sawCode = true;
            continue;
        }

        if (startsWith(line, ".data"))
        {
            mode = sectionData;
            continue;
        }

        if (*line == ':')
        {
            char *name;
            name = readLabelToken(line);
            pendingAdd(&pending, name);
            free(name);
            continue;
        }

        if (*line != '\t')
        {
            fclose(f);
            pendingFree(&pending);
            stopBuild("code/data line must start with tab character");
        }

        while (*line == '\t')
        {
            line++;
        }

        line = skipBlank(line);
        if (*line == '\0')
        {
            continue;
        }

        if (mode == sectionNone)
        {
            fclose(f);
            pendingFree(&pending);
            stopBuild("code/data line before any .code or .data directive");
        }

        if (mode == sectionData)
        {
            uint64_t v;

            v = 0;
            if (readU64Token(line, &v) == false)
            {
                fclose(f);
                pendingFree(&pending);
                stopBuild("malformed data item (expected 64-bit unsigned integer)");
            }

            addData(data, dataPc, v, &pending, labels);
            dataPc = dataPc + 8;
            continue;
        }

        {
            Words w;
            char *mn;

            w = splitLine(line);
            if (w.count == 0)
            {
                freeWords(&w);
                continue;
            }

            mn = w.items[0];
            {
                char *c;
                c = mn;
                while (*c != '\0')
                {
                    *c = (char)tolower((unsigned char)*c);
                    c++;
                }
            }

            requireCommaStyle(line, mn);

            if (strcmp(mn, "clr") == 0)
            {
                int rd;

                if (w.count != 2)
                {
                    freeWords(&w);
                    fclose(f);
                    pendingFree(&pending);
                    stopBuild("clr expects: clr rd");
                }

                rd = readReg(w.items[1]);
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
                int rd;
                int rs;

                if (w.count != 3)
                {
                    freeWords(&w);
                    fclose(f);
                    pendingFree(&pending);
                    stopBuild("in expects: in rd, rs");
                }

                rd = readReg(w.items[1]);
                rs = readReg(w.items[2]);

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
                int rd;
                int rs;

                if (w.count != 3)
                {
                    freeWords(&w);
                    fclose(f);
                    pendingFree(&pending);
                    stopBuild("out expects: out rd, rs");
                }

                rd = readReg(w.items[1]);
                rs = readReg(w.items[2]);

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
                int rd;

                if (w.count != 2)
                {
                    freeWords(&w);
                    fclose(f);
                    pendingFree(&pending);
                    stopBuild("push expects: push rd");
                }

                rd = readReg(w.items[1]);
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
                int rd;

                if (w.count != 2)
                {
                    freeWords(&w);
                    fclose(f);
                    pendingFree(&pending);
                    stopBuild("pop expects: pop rd");
                }

                rd = readReg(w.items[1]);
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

            if (strcmp(mn, "ld") == 0)
            {
                int rd;

                if (w.count != 3)
                {
                    freeWords(&w);
                    fclose(f);
                    pendingFree(&pending);
                    stopBuild("ld expects: ld rd, valueOrLabel");
                }

                rd = readReg(w.items[1]);
                if (rd < 0)
                {
                    freeWords(&w);
                    fclose(f);
                    pendingFree(&pending);
                    stopBuild("ld: invalid register");
                }

                if (w.items[2][0] == ':' && w.items[2][1] != '\0')
                {
                    uint64_t addr;

                    if (getLabel(labels, w.items[2] + 1, &addr) == false)
                    {
                        freeWords(&w);
                        fclose(f);
                        pendingFree(&pending);
                        stopBuildWithName("ld: undefined label: %s", w.items[2] + 1);
                    }

                    freeWords(&w);
                    emitLoad64(code, &codePc, rd, addr, &pending, labels);
                    continue;
                }

                {
                    uint64_t imm;

                    imm = 0;
                    if (readU64Token(w.items[2], &imm) == false)
                    {
                        freeWords(&w);
                        fclose(f);
                        pendingFree(&pending);
                        stopBuild("ld: invalid literal");
                    }

                    freeWords(&w);
                    emitLoad64(code, &codePc, rd, imm, &pending, labels);
                    continue;
                }
            }

            freeWords(&w);

            addText(code, codePc, line, &pending, labels);
            codePc = codePc + 4;
        }
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
    uint32_t *out;
    size_t i;

    out = (uint32_t *)malloc(sizeof(uint32_t) * code->count);
    if (out == NULL)
    {
        stopBuild("out of memory");
    }

    i = 0;
    while (i < code->count)
    {
        out[i] = assembleInstruction(code->items[i].text, code->items[i].address, labels);
        i++;
    }

    return out;
}

static void writeTko(const char *outPath, const ItemList *code, const ItemList *data, const uint32_t *words)
{
    FILE *f;
    uint64_t fileType;
    uint64_t codeBegin;
    uint64_t codeSize;
    uint64_t dataBegin;
    uint64_t dataSize;
    size_t i;

    f = fopen(outPath, "wb");
    if (f == NULL)
    {
        stopBuildWithName("cannot open output file: %s", outPath);
    }

    fileType = 0ULL;
    codeBegin = codeBase;
    codeSize = (uint64_t)(code->count * 4ULL);
    dataBegin = dataBase;
    dataSize = (uint64_t)(data->count * 8ULL);

    writeU64LE(f, fileType);
    writeU64LE(f, codeBegin);
    writeU64LE(f, codeSize);
    writeU64LE(f, dataBegin);
    writeU64LE(f, dataSize);

    i = 0;
    while (i < code->count)
    {
        writeU32LE(f, words[i]);
        i++;
    }

    i = 0;
    while (i < data->count)
    {
        writeU64LE(f, data->items[i].data);
        i++;
    }

    fclose(f);
}

int main(int argc, char **argv)
{
    const char *inputPath;
    const char *outputPath;

    ItemList code;
    ItemList data;
    LabelTable labels;
    uint32_t *words;

    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s input.tk output.tko\n", argv[0]);
        return 1;
    }

    inputPath = argv[1];
    outputPath = argv[2];

    code.items = NULL;
    code.count = 0;
    code.cap = 0;

    data.items = NULL;
    data.count = 0;
    data.cap = 0;

    labels.items = NULL;
    labels.count = 0;
    labels.cap = 0;

    buildProgram(inputPath, &code, &data, &labels);

    words = assembleAll(&code, &labels);
    writeTko(outputPath, &code, &data, words);

    free(words);
    freeItems(&code);
    freeItems(&data);
    freeLabels(&labels);

    return 0;
}