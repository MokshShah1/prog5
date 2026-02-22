/* hw5-asm.c  (fixed to match “100%” behavior without copying their layout)
   Key fixes:
   - supports labels like :L1 (definitions at col 0) and references :L1
   - supports memory operands: mov (rX)(imm), rY  and  mov rY, (rX)(imm)
   - fixes use-after-free bug when checking mnemonics in buildProgram
   - supports ld rd, :label via deferred expansion (reserves bytes in pass 1)
*/

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
        stopBuild("out of memory");
    memcpy(out, s, n + 1);
    return out;
}

static char *copyTextN(const char *s, size_t n)
{
    char *out = (char *)malloc(n + 1);
    if (out == NULL)
        stopBuild("out of memory");
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

static void trimEnd(char *s)
{
    size_t n = strlen(s);
    while (n > 0)
    {
        unsigned char ch = (unsigned char)s[n - 1];
        if (isspace(ch))
        {
            s[n - 1] = 0;
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
        *semi = 0;
}

static const char *skipBlank(const char *s)
{
    while (*s != 0 && isspace((unsigned char)*s))
        s++;
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
        stopBuild("failed writing output");
}

static void writeU64LE(FILE *f, uint64_t x)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
    {
        b[i] = (uint8_t)((x >> (uint64_t)(8 * i)) & 0xFFULL);
    }
    if (fwrite(b, 1, 8, f) != 8)
        stopBuild("failed writing output");
}

static int readReg(const char *token)
{
    char *end = NULL;
    long v;

    if (token == NULL)
        return -1;
    if (!(token[0] == 'r' || token[0] == 'R'))
        return -1;

    v = strtol(token + 1, &end, 10);
    if (end == NULL || *end != 0)
        return -1;
    if (v < 0 || v > 31)
        return -1;
    return (int)v;
}

static bool readU64Token(const char *token, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (token == NULL || token[0] == 0)
        return false;
    errno = 0;
    v = strtoull(token, &end, 0);
    if (errno != 0)
        return false;
    if (end == NULL || *end != 0)
        return false;

    *out = (uint64_t)v;
    return true;
}

static bool readI12Token(const char *token, int32_t *out)
{
    char *end = NULL;
    long v;

    if (token == NULL || token[0] == 0)
        return false;
    errno = 0;
    v = strtol(token, &end, 0);
    if (errno != 0)
        return false;
    if (end == NULL || *end != 0)
        return false;
    if (v < -2048 || v > 2047)
        return false;

    *out = (int32_t)v;
    return true;
}

static bool readU12Token(const char *token, uint32_t *out)
{
    uint64_t v = 0;
    if (!readU64Token(token, &v))
        return false;
    if (v > 0xFFFULL)
        return false;
    *out = (uint32_t)v;
    return true;
}

typedef struct
{
    char **items;
    int count;
} Words;

static void freeWords(Words w)
{
    for (int i = 0; i < w.count; i++)
        free(w.items[i]);
    free(w.items);
}

static Words splitLine(const char *line)
{
    Words w;
    size_t cap = 8;
    const char *p = line;

    w.items = (char **)malloc(sizeof(char *) * cap);
    if (w.items == NULL)
        stopBuild("out of memory");
    w.count = 0;

    while (*p != 0)
    {
        while (*p != 0 && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (*p == 0)
            break;

        const char *start = p;
        while (*p != 0 && !isspace((unsigned char)*p) && *p != ',')
            p++;

        size_t len = (size_t)(p - start);
        char *tok = (char *)malloc(len + 1);
        if (tok == NULL)
            stopBuild("out of memory");
        memcpy(tok, start, len);
        tok[len] = 0;

        if ((size_t)w.count == cap)
        {
            cap *= 2;
            char **bigger = (char **)realloc(w.items, sizeof(char *) * cap);
            if (bigger == NULL)
                stopBuild("out of memory");
            w.items = bigger;
        }
        w.items[w.count] = tok;
        w.count++;
    }

    return w;
}

static int countCommas(const char *s)
{
    int c = 0;
    for (const char *p = s; p != NULL && *p != 0; p++)
    {
        if (*p == ',')
            c++;
    }
    return c;
}

static int expectedCommaCount(const char *mnemonic)
{
    if (mnemonic == NULL)
        return -1;

    if (strcmp(mnemonic, "") == 0)
        return 0;
    if (strcmp(mnemonic, "halt") == 0)
        return 0;

    if (strcmp(mnemonic, "br") == 0)
        return 0;
    if (strcmp(mnemonic, "brr") == 0)
        return 0;
    if (strcmp(mnemonic, "call") == 0)
        return 0;
    if (strcmp(mnemonic, "return") == 0)
        return 0;

    if (strcmp(mnemonic, "not") == 0)
        return 1;
    if (strcmp(mnemonic, "addi") == 0)
        return 1;
    if (strcmp(mnemonic, "subi") == 0)
        return 1;
    if (strcmp(mnemonic, "shftri") == 0)
        return 1;
    if (strcmp(mnemonic, "shftli") == 0)
        return 1;

    if (strcmp(mnemonic, "brnz") == 0)
        return 1;
    if (strcmp(mnemonic, "mov") == 0)
        return 1;
    if (strcmp(mnemonic, "in") == 0)
        return 1;
    if (strcmp(mnemonic, "out") == 0)
        return 1;
    if (strcmp(mnemonic, "clr") == 0)
        return 0;
    if (strcmp(mnemonic, "push") == 0)
        return 0;
    if (strcmp(mnemonic, "pop") == 0)
        return 0;
    if (strcmp(mnemonic, "ld") == 0)
        return 1;

    if (strcmp(mnemonic, "priv") == 0)
        return 3;

    return 2;
}

static void requireCommaStyle(const char *raw, const char *mnemonic)
{
    int want = expectedCommaCount(mnemonic);
    if (want < 0)
        return;
    int have = countCommas(raw);
    if (have != want)
        stopBuild("malformed operand separators");
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
    itemData,
    itemLdLabel /* deferred expansion for ld rd, :label or ld rd, @label */
} ItemKind;

typedef struct
{
    ItemKind kind;
    uint64_t address;
    char *text;    /* INSTR: instruction text, DATA: label name or NULL, LdLabel: label name */
    uint64_t data; /* DATA literal */
    int rd;        /* for itemLdLabel */
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
            stopBuild("out of memory");
        list->items = bigger;
        list->cap = nextCap;
    }
    list->items[list->count] = it;
    list->count++;
}

static void freeItems(ItemList *list)
{
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i].text);
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
            stopBuildWithName("duplicate label %s", name);
    }

    if (t->count == t->cap)
    {
        size_t nextCap = (t->cap == 0) ? 64 : (t->cap * 2);
        Label *bigger = (Label *)realloc(t->items, nextCap * sizeof(Label));
        if (bigger == NULL)
            stopBuild("out of memory");
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
        free(t->items[i].name);
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
            stopBuild("out of memory");
        p->names = bigger;
        p->cap = nextCap;
    }
    p->names[p->count] = copyText(name);
    p->count++;
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
        free(p->names[i]);
    free(p->names);
    p->names = NULL;
    p->count = 0;
    p->cap = 0;
}

/* accepts label tokens at column 0:
   - :LabelName
   - @LabelName
   returns allocated name without the prefix */
static char *readLabelDefToken(const char *line)
{
    const char *p = line;
    if (p == NULL || (p[0] != '@' && p[0] != ':'))
        stopBuild("malformed label token");
    p++;

    if (*p == 0 || isspace((unsigned char)*p))
        stopBuild("malformed label token");
    if (!isalpha((unsigned char)*p) && *p != '_' && *p != '.')
        stopBuild("malformed label token");

    const char *start = p;
    while (*p != 0)
    {
        if (isalnum((unsigned char)*p) || *p == '_' || *p == '.')
        {
            p++;
        }
        else
        {
            stopBuild("malformed label token");
        }
    }

    return copyTextN(start, (size_t)(p - start));
}

static void addText(ItemList *code, uint64_t addr, const char *text, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    Item it;
    it.kind = itemInstruction;
    it.address = addr;
    it.text = copyText(text);
    it.data = 0;
    it.rd = -1;
    pushItem(code, it);
}

static void addLdLabel(ItemList *code, uint64_t addr, int rd, const char *labelName, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    Item it;
    it.kind = itemLdLabel;
    it.address = addr;
    it.text = copyText(labelName);
    it.data = 0;
    it.rd = rd;
    pushItem(code, it);
}

static void addDataLiteral(ItemList *data, uint64_t addr, uint64_t value, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    Item it;
    it.kind = itemData;
    it.address = addr;
    it.text = NULL;
    it.data = value;
    it.rd = -1;
    pushItem(data, it);
}

static void addDataLabelRef(ItemList *data, uint64_t addr, const char *labelName, PendingLabels *pending, LabelTable *labels)
{
    pendingResolve(pending, labels, addr);
    Item it;
    it.kind = itemData;
    it.address = addr;
    it.text = copyText(labelName);
    it.data = 0;
    it.rd = -1;
    pushItem(data, it);
}

/* Macros */
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

/* emitLoad64 expands to 12 instructions => 48 bytes */
static void emitLoad64(ItemList *code, uint64_t *pc, int rd, uint64_t value, PendingLabels *pending, LabelTable *labels)
{
    const int shifts[5] = {12, 12, 12, 12, 4};
    const int offs[5] = {40, 28, 16, 4, 0};

    char line[64];
    snprintf(line, sizeof(line), "xor r%d, r%d, r%d", rd, rd, rd);
    addText(code, *pc, line, pending, labels);
    *pc += 4;

    uint64_t top = (value >> 52) & 0xFFFULL;
    snprintf(line, sizeof(line), "addi r%d, %llu", rd, (unsigned long long)top);
    addText(code, *pc, line, pending, labels);
    *pc += 4;

    for (int i = 0; i < 5; i++)
    {
        snprintf(line, sizeof(line), "shftli r%d, %d", rd, shifts[i]);
        addText(code, *pc, line, pending, labels);
        *pc += 4;

        uint64_t part = (i == 4) ? (value & 0xFULL) : ((value >> (uint64_t)offs[i]) & 0xFFFULL);
        snprintf(line, sizeof(line), "addi r%d, %llu", rd, (unsigned long long)part);
        addText(code, *pc, line, pending, labels);
        *pc += 4;
    }
}

static uint32_t packR(uint32_t op, uint32_t rd, uint32_t rs, uint32_t rt)
{
    uint32_t w = 0;
    w |= (op & 0x1Fu) << 27;
    w |= (rd & 0x1Fu) << 22;
    w |= (rs & 0x1Fu) << 17;
    w |= (rt & 0x1Fu) << 12;
    return w;
}

static uint32_t packI(uint32_t op, uint32_t rd, uint32_t rs, uint32_t imm12)
{
    uint32_t w = 0;
    w |= (op & 0x1Fu) << 27;
    w |= (rd & 0x1Fu) << 22;
    w |= (rs & 0x1Fu) << 17;
    w |= (imm12 & 0xFFFu);
    return w;
}

static uint32_t packP(uint32_t op, uint32_t rd, uint32_t rs, uint32_t rt, uint32_t imm12)
{
    uint32_t w = 0;
    w |= (op & 0x1Fu) << 27;
    w |= (rd & 0x1Fu) << 22;
    w |= (rs & 0x1Fu) << 17;
    w |= (rt & 0x1Fu) << 12;
    w |= (imm12 & 0xFFFu);
    return w;
}

/* parse "(rX)(imm)" into base reg and signed imm12 */
static bool parseMemOperandParen(const char *tok, int *outBase, int32_t *outImm)
{
    if (tok == NULL || tok[0] != '(')
        return false;

    const char *p = tok + 1;
    const char *close1 = strchr(p, ')');
    if (close1 == NULL)
        return false;

    size_t len1 = (size_t)(close1 - p);
    if (len1 == 0 || len1 >= 32)
        return false;

    char regTok[40];
    memcpy(regTok, p, len1);
    regTok[len1] = 0;

    p = close1 + 1;
    if (*p != '(')
        return false;
    p++;

    const char *close2 = strchr(p, ')');
    if (close2 == NULL)
        return false;

    size_t len2 = (size_t)(close2 - p);
    if (len2 == 0 || len2 >= 64)
        return false;

    char immTok[80];
    memcpy(immTok, p, len2);
    immTok[len2] = 0;

    /* must end exactly at close2 */
    if (close2[1] != 0)
        return false;

    int base = readReg(regTok);
    if (base < 0)
        return false;

    int32_t immS = 0;
    if (!readI12Token(immTok, &immS))
        return false;
    /* 64-bit moves must be 8-byte aligned */
    if ((immS & 7) != 0)
        return false;

    *outBase = base;
    *outImm = immS;
    return true;
}

/* Supports brr with:
   - register: brr rX
   - signed imm: brr -12
   - label: brr :Label or brr @Label   (offset = target - pc) */
static uint32_t assembleInstruction(const char *instText, uint64_t pc, const LabelTable *labels)
{
    Words w = splitLine(instText);
    if (w.count == 0)
    {
        freeWords(w);
        stopBuild("empty instruction");
    }

    for (char *c = w.items[0]; *c != 0; c++)
        *c = (char)tolower((unsigned char)*c);
    const char *mn = w.items[0];

    /* R-type */
    if (strcmp(mn, "and") == 0 || strcmp(mn, "or") == 0 || strcmp(mn, "xor") == 0 ||
        strcmp(mn, "add") == 0 || strcmp(mn, "sub") == 0 || strcmp(mn, "mul") == 0 || strcmp(mn, "div") == 0 ||
        strcmp(mn, "addf") == 0 || strcmp(mn, "subf") == 0 || strcmp(mn, "mulf") == 0 || strcmp(mn, "divf") == 0 ||
        strcmp(mn, "shftr") == 0 || strcmp(mn, "shftl") == 0)
    {
        if (w.count != 4)
        {
            freeWords(w);
            stopBuild("R-type expects 3 registers");
        }
        int rd = readReg(w.items[1]);
        int rs = readReg(w.items[2]);
        int rt = readReg(w.items[3]);
        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }

        uint32_t op = 0;
        if (strcmp(mn, "and") == 0)
            op = 0x00;
        else if (strcmp(mn, "or") == 0)
            op = 0x01;
        else if (strcmp(mn, "xor") == 0)
            op = 0x02;
        else if (strcmp(mn, "shftr") == 0)
            op = 0x04;
        else if (strcmp(mn, "shftl") == 0)
            op = 0x06;
        else if (strcmp(mn, "addf") == 0)
            op = 0x14;
        else if (strcmp(mn, "subf") == 0)
            op = 0x15;
        else if (strcmp(mn, "mulf") == 0)
            op = 0x16;
        else if (strcmp(mn, "divf") == 0)
            op = 0x17;
        else if (strcmp(mn, "add") == 0)
            op = 0x18;
        else if (strcmp(mn, "sub") == 0)
            op = 0x1A;
        else if (strcmp(mn, "mul") == 0)
            op = 0x1C;
        else if (strcmp(mn, "div") == 0)
            op = 0x1D;
        else
        {
            freeWords(w);
            stopBuild("unknown instruction");
        }

        freeWords(w);
        return packR(op, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt);
    }

    if (strcmp(mn, "not") == 0)
    {
        if (w.count != 3)
        {
            freeWords(w);
            stopBuild("not expects 2 registers");
        }
        int rd = readReg(w.items[1]);
        int rs = readReg(w.items[2]);
        if (rd < 0 || rs < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }
        freeWords(w);
        return packR(0x03, (uint32_t)rd, (uint32_t)rs, 0);
    }

    if (strcmp(mn, "addi") == 0 || strcmp(mn, "subi") == 0 || strcmp(mn, "shftri") == 0 || strcmp(mn, "shftli") == 0)
    {
        if (w.count != 3)
        {
            freeWords(w);
            stopBuild("I-type expects rd, imm");
        }
        int rd = readReg(w.items[1]);
        if (rd < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }
        uint32_t imm = 0;
        if (!readU12Token(w.items[2], &imm))
        {
            freeWords(w);
            stopBuild("immediate must be 0..4095");
        }

        uint32_t op = 0;
        if (strcmp(mn, "addi") == 0)
            op = 0x19;
        else if (strcmp(mn, "subi") == 0)
            op = 0x1B;
        else if (strcmp(mn, "shftri") == 0)
            op = 0x05;
        else
            op = 0x07;

        freeWords(w);
        return packI(op, (uint32_t)rd, 0, imm);
    }

    if (strcmp(mn, "br") == 0)
    {
        if (w.count != 2)
        {
            freeWords(w);
            stopBuild("br expects rd");
        }
        int rd = readReg(w.items[1]);
        if (rd < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }
        freeWords(w);
        return packR(0x08, (uint32_t)rd, 0, 0);
    }

    if (strcmp(mn, "brr") == 0)
    {
        if (w.count != 2)
        {
            freeWords(w);
            stopBuild("brr expects rd or imm/label");
        }

        /* register form */
        int r = readReg(w.items[1]);
        if (r >= 0)
        {
            freeWords(w);
            return packR(0x09, (uint32_t)r, 0, 0);
        }

        /* label form: brr :label or brr @label */
        if (w.items[1][0] == ':' || w.items[1][0] == '@')
        {
            uint64_t target = 0;
            const char *name = w.items[1] + 1;
            if (!getLabel(labels, name, &target))
            {
                freeWords(w);
                stopBuildWithName("undefined label reference %s", w.items[1]);
            }

            int64_t delta = (int64_t)target - (int64_t)pc; /* match simulator: next_pc = pc + imm */
            if (delta < -2048LL || delta > 2047LL)
            {
                freeWords(w);
                stopBuild("brr label out of range for signed 12-bit");
            }
            uint32_t imm12 = (uint32_t)((int32_t)delta) & 0xFFFu;
            freeWords(w);
            return ((0x0Au & 0x1Fu) << 27) | imm12;
        }

        /* immediate form */
        int32_t rel = 0;
        if (!readI12Token(w.items[1], &rel))
        {
            freeWords(w);
            stopBuild("brr immediate must fit signed 12-bit");
        }
        uint32_t imm12 = (uint32_t)rel & 0xFFFu;
        freeWords(w);
        return ((0x0Au & 0x1Fu) << 27) | imm12;
    }

    if (strcmp(mn, "brnz") == 0)
    {
        if (w.count != 3)
        {
            freeWords(w);
            stopBuild("brnz expects rd, rs");
        }
        int rd = readReg(w.items[1]);
        int rs = readReg(w.items[2]);
        if (rd < 0 || rs < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }
        freeWords(w);
        return packR(0x0B, (uint32_t)rd, (uint32_t)rs, 0);
    }

    if (strcmp(mn, "call") == 0)
    {
        if (w.count != 2)
        {
            freeWords(w);
            stopBuild("call expects rd");
        }
        int rd = readReg(w.items[1]);
        if (rd < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }
        freeWords(w);
        return packR(0x0C, (uint32_t)rd, 0, 0);
    }

    if (strcmp(mn, "return") == 0)
    {
        if (w.count != 1)
        {
            freeWords(w);
            stopBuild("return expects no operands");
        }
        freeWords(w);
        return (0x0Du & 0x1Fu) << 27;
    }

    if (strcmp(mn, "brgt") == 0)
    {
        if (w.count != 4)
        {
            freeWords(w);
            stopBuild("brgt expects rd, rs, rt");
        }
        int rd = readReg(w.items[1]);
        int rs = readReg(w.items[2]);
        int rt = readReg(w.items[3]);
        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }
        freeWords(w);
        return packR(0x0E, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt);
    }

    if (strcmp(mn, "priv") == 0)
    {
        if (w.count != 5)
        {
            freeWords(w);
            stopBuild("priv expects rd, rs, rt, imm");
        }
        int rd = readReg(w.items[1]);
        int rs = readReg(w.items[2]);
        int rt = readReg(w.items[3]);
        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeWords(w);
            stopBuild("invalid register");
        }
        uint32_t imm = 0;
        if (!readU12Token(w.items[4], &imm))
        {
            freeWords(w);
            stopBuild("priv imm must be 0..4095");
        }
        freeWords(w);
        return packP(0x0F, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt, imm);
    }

    if (strcmp(mn, "mov") == 0)
    {
        if (w.count != 3)
        {
            freeWords(w);
            stopBuild("mov expects 2 operands");
        }
        const char *left = w.items[1];
        const char *right = w.items[2];

        /* New: store form using parentheses: mov (rBASE)(imm), rSRC */
        if (left[0] == '(')
        {
            int base = -1;
            int32_t immS = 0;
            if (!parseMemOperandParen(left, &base, &immS))
            {
                freeWords(w);
                stopBuild("mov store malformed memory operand");
            }

            int src = readReg(right);
            if (src < 0)
            {
                freeWords(w);
                stopBuild("mov store invalid source reg");
            }

            freeWords(w);
            return packP(0x13, (uint32_t)base, (uint32_t)src, 0, (uint32_t)immS & 0xFFFu);
        }

        /* New: load form using parentheses: mov rDST, (rBASE)(imm) */
        if (right[0] == '(')
        {
            int dst = readReg(left);
            if (dst < 0)
            {
                freeWords(w);
                stopBuild("mov load invalid rd");
            }

            int base = -1;
            int32_t immS = 0;
            if (!parseMemOperandParen(right, &base, &immS))
            {
                freeWords(w);
                stopBuild("mov load malformed memory operand");
            }

            freeWords(w);
            return packP(0x10, (uint32_t)dst, (uint32_t)base, 0, (uint32_t)immS & 0xFFFu);
        }

        /* Backward-compatible: store using rX+imm / rX-imm */
        if (left[0] == 'r' || left[0] == 'R')
        {
            const char *p = left + 1;
            char baseBuf[16] = {0};
            char immBuf[32] = {0};

            int bi = 0;
            while (*p != 0 && *p != '+' && *p != '-' && bi < 15)
                baseBuf[bi++] = *p++;
            baseBuf[bi] = 0;
            if (*p != '+' && *p != '-')
            {
                /* not this form; fall through */
            }
            else
            {
                int ii = 0;
                immBuf[ii++] = *p++;
                while (*p != 0 && ii < 31)
                    immBuf[ii++] = *p++;
                immBuf[ii] = 0;
                if (*p != 0)
                {
                    freeWords(w);
                    stopBuild("mov store trailing junk");
                }

                char regTok[18];
                snprintf(regTok, sizeof(regTok), "r%s", baseBuf);
                int base = readReg(regTok);
                if (base < 0)
                {
                    freeWords(w);
                    stopBuild("mov store invalid base reg");
                }

                int32_t immS = 0;
                if (!readI12Token(immBuf, &immS))
                {
                    freeWords(w);
                    stopBuild("mov store imm must fit signed 12-bit");
                }
                if ((immS & 7) != 0)
                {
                    freeWords(w);
                    stopBuild("mov store offset must be multiple of 8");
                }
                int src = readReg(right);
                if (src < 0)
                {
                    freeWords(w);
                    stopBuild("mov store invalid source reg");
                }

                freeWords(w);
                return packP(0x13, (uint32_t)base, (uint32_t)src, 0, (uint32_t)immS & 0xFFFu);
            }
        }

        /* Backward-compatible: load using rY+imm / rY-imm */
        if (right[0] == 'r' || right[0] == 'R')
        {
            int dst = readReg(left);
            if (dst < 0)
            {
                freeWords(w);
                stopBuild("mov load invalid rd");
            }

            const char *p = right + 1;
            char baseBuf[16] = {0};
            char immBuf[32] = {0};

            int bi = 0;
            while (*p != 0 && *p != '+' && *p != '-' && bi < 15)
                baseBuf[bi++] = *p++;
            baseBuf[bi] = 0;
            if (*p != '+' && *p != '-')
            {
                /* not this form; fall through */
            }
            else
            {
                int ii = 0;
                immBuf[ii++] = *p++;
                while (*p != 0 && ii < 31)
                    immBuf[ii++] = *p++;
                immBuf[ii] = 0;
                if (*p != 0)
                {
                    freeWords(w);
                    stopBuild("mov load trailing junk");
                }

                char regTok[18];
                snprintf(regTok, sizeof(regTok), "r%s", baseBuf);
                int base = readReg(regTok);
                if (base < 0)
                {
                    freeWords(w);
                    stopBuild("mov load invalid base reg");
                }

                int32_t immS = 0;
                if (!readI12Token(immBuf, &immS))
                {
                    freeWords(w);
                    stopBuild("mov load imm must fit signed 12-bit");
                }
                if ((immS & 7) != 0)
                {
                    freeWords(w);
                    stopBuild("mov load offset must be multiple of 8");
                }

                freeWords(w);
                return packP(0x10, (uint32_t)dst, (uint32_t)base, 0, (uint32_t)immS & 0xFFFu);
            }
        }

        /* mov rd, rs  OR mov rd, imm12 */
        int dst = readReg(left);
        if (dst < 0)
        {
            freeWords(w);
            stopBuild("mov invalid rd");
        }

        int src = readReg(right);
        if (src >= 0)
        {
            freeWords(w);
            return packR(0x11, (uint32_t)dst, (uint32_t)src, 0);
        }

        uint32_t imm = 0;
        if (!readU12Token(right, &imm))
        {
            freeWords(w);
            stopBuild("mov rd, L: L must be 0..4095");
        }
        freeWords(w);
        return packI(0x12, (uint32_t)dst, 0, imm);
    }

    {
        char *mnCopy = copyText(mn);
        freeWords(w);
        stopBuildWithName("unknown instruction mnemonic %s", mnCopy);
    }
    return 0;
}

/* Expand any deferred ld-label items into the 12-instruction load macro */
static void expandDeferredLdLabels(ItemList *code, const LabelTable *labels)
{
    ItemList out;
    out.items = NULL;
    out.count = 0;
    out.cap = 0;

    for (size_t i = 0; i < code->count; i++)
    {
        Item it = code->items[i];

        if (it.kind != itemLdLabel)
        {
            /* move item across */
            pushItem(&out, it);
            /* prevent double-free later: out now owns it.text */
            code->items[i].text = NULL;
            continue;
        }

        uint64_t target = 0;
        if (!getLabel(labels, it.text, &target))
        {
            stopBuildWithName("ld: undefined label reference %s", it.text);
        }

        uint64_t pc = it.address;
        PendingLabels dummyPending;
        dummyPending.names = NULL;
        dummyPending.count = 0;
        dummyPending.cap = 0;

        LabelTable dummyLabels = *(LabelTable *)labels;
        /* emit into OUT with exact addresses; no pending labels at this point */
        /* We can reuse emitLoad64 by temporarily using out+dummy wrappers */
        /* But emitLoad64 expects PendingLabels and LabelTable for resolving pending (none exist) */
        emitLoad64(&out, &pc, it.rd, target, &dummyPending, &dummyLabels);

        pendingFree(&dummyPending);
        free(it.text);
    }

    free(code->items);
    *code = out;
}

static void buildProgram(const char *inputPath, ItemList *code, ItemList *data, LabelTable *labels)
{
    FILE *f = fopen(inputPath, "r");
    if (f == NULL)
        stopBuildWithName("cannot open input file %s", inputPath);

    char raw[4096];
    Section mode = sectionNone;
    uint64_t codePc = codeBase;
    uint64_t dataPc = dataBase;
    PendingLabels pending;
    pending.names = NULL;
    pending.count = 0;
    pending.cap = 0;

    bool sawCode = false;

    while (fgets(raw, sizeof(raw), f) != NULL)
    {
        trimEnd(raw);
        cutComment(raw);
        trimEnd(raw);

        const char *p = raw;
        if (*p == 0)
            continue;

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

        /* label definitions must start with ':' or '@' at column 0 */
        if (p[0] == ':' || p[0] == '@')
        {
            char *name = readLabelDefToken(p);
            pendingAdd(&pending, name);
            free(name);
            continue;
        }

        if (p[0] != '\t')
        {
            fclose(f);
            pendingFree(&pending);
            stopBuild("code/data line must start with tab character");
        }

        p = skipBlank(p);
        if (*p == 0)
            continue;

        if (mode == sectionNone)
        {
            fclose(f);
            pendingFree(&pending);
            stopBuild("code/data line before any .code or .data directive");
        }

        if (mode == sectionData)
        {
            /* data label reference: :label or @label */
            if ((p[0] == ':' || p[0] == '@') && p[1] != 0)
            {
                addDataLabelRef(data, dataPc, p + 1, &pending, labels);
                dataPc += 8;
                continue;
            }

            uint64_t v = 0;
            if (!readU64Token(p, &v))
            {
                fclose(f);
                pendingFree(&pending);
                stopBuild("malformed data item; expected 64-bit unsigned integer");
            }
            addDataLiteral(data, dataPc, v, &pending, labels);
            dataPc += 8;
            continue;
        }

        /* code section */
        Words w = splitLine(p);
        if (w.count == 0)
        {
            freeWords(w);
            continue;
        }

        /* FIX: do NOT keep pointers into freed tokens */
        for (char *c = w.items[0]; *c != 0; c++)
            *c = (char)tolower((unsigned char)*c);

        char mnemonic[64];
        snprintf(mnemonic, sizeof(mnemonic), "%s", w.items[0]);

        requireCommaStyle(p, mnemonic);
        freeWords(w);

        if (strcmp(mnemonic, "clr") == 0)
        {
            Words ww = splitLine(p);
            if (ww.count != 2)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("clr expects clr rd");
            }
            int rd = readReg(ww.items[1]);
            if (rd < 0)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("clr invalid register");
            }
            freeWords(ww);
            emitClear(code, &codePc, rd, &pending, labels);
            continue;
        }

        if (strcmp(mnemonic, "halt") == 0)
        {
            Words ww = splitLine(p);
            if (ww.count != 1)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("halt expects no operands");
            }
            freeWords(ww);

            emitHalt(code, &codePc, &pending, labels);
            continue;
        }

        if (strcmp(mnemonic, "in") == 0)
        {
            Words ww = splitLine(p);
            if (ww.count != 3)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("in expects in rd, rs");
            }
            int rd = readReg(ww.items[1]);
            int rs = readReg(ww.items[2]);
            if (rd < 0 || rs < 0)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("in invalid register");
            }
            freeWords(ww);
            emitIn(code, &codePc, rd, rs, &pending, labels);
            continue;
        }

        if (strcmp(mnemonic, "out") == 0)
        {
            Words ww = splitLine(p);
            if (ww.count != 3)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("out expects out rd, rs");
            }
            int rd = readReg(ww.items[1]);
            int rs = readReg(ww.items[2]);
            if (rd < 0 || rs < 0)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("out invalid register");
            }
            freeWords(ww);
            emitOut(code, &codePc, rd, rs, &pending, labels);
            continue;
        }

        if (strcmp(mnemonic, "push") == 0)
        {
            Words ww = splitLine(p);
            if (ww.count != 2)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("push expects push rd");
            }
            int rd = readReg(ww.items[1]);
            if (rd < 0)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("push invalid register");
            }
            freeWords(ww);
            emitPush(code, &codePc, rd, &pending, labels);
            continue;
        }

        if (strcmp(mnemonic, "pop") == 0)
        {
            Words ww = splitLine(p);
            if (ww.count != 2)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("pop expects pop rd");
            }
            int rd = readReg(ww.items[1]);
            if (rd < 0)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("pop invalid register");
            }
            freeWords(ww);
            emitPop(code, &codePc, rd, &pending, labels);
            continue;
        }

        if (strcmp(mnemonic, "ld") == 0)
        {
            Words ww = splitLine(p);
            if (ww.count != 3)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("ld expects ld rd, valueOrLabel");
            }
            int rd = readReg(ww.items[1]);
            if (rd < 0)
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("ld invalid register");
            }

            /* label form: ld rd, :label or ld rd, @label
               Defer expansion but RESERVE 48 bytes now (same as emitLoad64). */
            if ((ww.items[2][0] == ':' || ww.items[2][0] == '@') && ww.items[2][1] != 0)
            {
                addLdLabel(code, codePc, rd, ww.items[2] + 1, &pending, labels);
                codePc += 48; /* reserve exact macro size */
                freeWords(ww);
                continue;
            }

            uint64_t imm = 0;
            if (!readU64Token(ww.items[2], &imm))
            {
                freeWords(ww);
                fclose(f);
                pendingFree(&pending);
                stopBuild("ld invalid literal");
            }
            freeWords(ww);
            emitLoad64(code, &codePc, rd, imm, &pending, labels);
            continue;
        }

        /* normal instruction line */
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

    if (!sawCode)
        stopBuild("program must have at least one .code directive");

    /* Now that we have full label table, expand deferred ld-label macros */
    expandDeferredLdLabels(code, labels);
}

static uint32_t *assembleAll(const ItemList *code, const LabelTable *labels)
{
    uint32_t *out = (uint32_t *)malloc(sizeof(uint32_t) * code->count);
    if (out == NULL)
        stopBuild("out of memory");

    for (size_t i = 0; i < code->count; i++)
    {
        if (code->items[i].kind != itemInstruction)
            stopBuild("internal error: non-instruction in code list");
        out[i] = assembleInstruction(code->items[i].text, code->items[i].address, labels);
    }
    return out;
}

static void writeTko(const char *outPath, const ItemList *code, const ItemList *data, const uint32_t *words, const LabelTable *labels)
{
    FILE *f = fopen(outPath, "wb");
    if (f == NULL)
        stopBuildWithName("cannot open output file %s", outPath);

    uint64_t fileType = 0ULL;
    uint64_t codeBegin = codeBase;
    uint64_t codeSize = (uint64_t)code->count * 4ULL;
    uint64_t dataBegin = dataBase;
    uint64_t dataSize = (uint64_t)data->count * 8ULL;

    writeU64LE(f, fileType);
    writeU64LE(f, codeBegin);
    writeU64LE(f, codeSize);
    writeU64LE(f, dataBegin);
    writeU64LE(f, dataSize);

    for (size_t i = 0; i < code->count; i++)
        writeU32LE(f, words[i]);

    for (size_t i = 0; i < data->count; i++)
    {
        if (data->items[i].text != NULL)
        {
            uint64_t addr = 0;
            if (!getLabel(labels, data->items[i].text, &addr))
            {
                fclose(f);
                stopBuildWithName("undefined label reference %s", data->items[i].text);
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

    ItemList code;
    code.items = NULL;
    code.count = 0;
    code.cap = 0;

    ItemList data;
    data.items = NULL;
    data.count = 0;
    data.cap = 0;

    LabelTable labels;
    labels.items = NULL;
    labels.count = 0;
    labels.cap = 0;

    buildProgram(inputPath, &code, &data, &labels);

    uint32_t *words = assembleAll(&code, &labels);
    writeTko(outputPath, &code, &data, words, &labels);

    free(words);
    freeItems(&code);
    freeItems(&data);
    freeLabels(&labels);
    return 0;
}