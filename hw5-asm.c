#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static const uint64_t programCodeBase = 0x2000ULL;
static const uint64_t programDataBase = 0x10000ULL;

static void failBuild(const char *message)
{
    fprintf(stderr, "Error: %s\n", message);
    exit(1);
}

static void failBuildWithName(const char *format, const char *name)
{
    fprintf(stderr, "Error: ");
    fprintf(stderr, format, name);
    fprintf(stderr, "\n");
    exit(1);
}

static char *duplicateText(const char *text)
{
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL)
    {
        failBuild("out of memory");
    }
    memcpy(copy, text, length + 1);
    return copy;
}

static char *duplicateTextN(const char *text, size_t length)
{
    char *copy = (char *)malloc(length + 1);
    if (copy == NULL)
    {
        failBuild("out of memory");
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static void rstripWhitespace(char *text)
{
    size_t length = strlen(text);
    while (length > 0)
    {
        unsigned char ch = (unsigned char)text[length - 1];
        if (isspace(ch))
        {
            text[length - 1] = '\0';
            length--;
        }
        else
        {
            break;
        }
    }
}

static void stripSemicolonComment(char *text)
{
    char *semicolon = strchr(text, ';');
    if (semicolon != NULL)
    {
        *semicolon = '\0';
    }
}

static const char *skipLeadingWhitespace(const char *text)
{
    const char *p = text;
    while (*p != '\0' && isspace((unsigned char)*p))
    {
        p++;
    }
    return p;
}

static bool hasPrefix(const char *text, const char *prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void writeU32LittleEndian(FILE *file, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value & 0xFFu);
    bytes[1] = (uint8_t)((value >> 8) & 0xFFu);
    bytes[2] = (uint8_t)((value >> 16) & 0xFFu);
    bytes[3] = (uint8_t)((value >> 24) & 0xFFu);

    if (fwrite(bytes, 1, 4, file) != 4)
    {
        failBuild("failed writing output");
    }
}

static void writeU64LittleEndian(FILE *file, uint64_t value)
{
    uint8_t bytes[8];
    int i = 0;

    for (i = 0; i < 8; i++)
    {
        bytes[i] = (uint8_t)((value >> (uint64_t)(8 * i)) & 0xFFULL);
    }

    if (fwrite(bytes, 1, 8, file) != 8)
    {
        failBuild("failed writing output");
    }
}

static int readRegisterNumber(const char *token)
{
    char *end = NULL;
    long number = 0;

    if (token == NULL)
    {
        return -1;
    }

    if (!(token[0] == 'r' || token[0] == 'R'))
    {
        return -1;
    }

    number = strtol(token + 1, &end, 10);

    if (end == NULL || *end != '\0')
    {
        return -1;
    }

    if (number < 0 || number > 31)
    {
        return -1;
    }

    return (int)number;
}

static bool readUnsigned64(const char *token, uint64_t *outValue)
{
    char *end = NULL;
    unsigned long long number = 0;

    if (token == NULL || token[0] == '\0')
    {
        return false;
    }

    errno = 0;
    number = strtoull(token, &end, 0);

    if (errno != 0)
    {
        return false;
    }

    if (end == NULL || *end != '\0')
    {
        return false;
    }

    *outValue = (uint64_t)number;
    return true;
}

static bool readSigned12(const char *token, int32_t *outValue)
{
    char *end = NULL;
    long number = 0;

    if (token == NULL || token[0] == '\0')
    {
        return false;
    }

    errno = 0;
    number = strtol(token, &end, 0);

    if (errno != 0)
    {
        return false;
    }

    if (end == NULL || *end != '\0')
    {
        return false;
    }

    if (number < -2048 || number > 2047)
    {
        return false;
    }

    *outValue = (int32_t)number;
    return true;
}

static bool readUnsigned12(const char *token, uint32_t *outValue)
{
    uint64_t number = 0;

    if (!readUnsigned64(token, &number))
    {
        return false;
    }

    if (number > 0xFFFULL)
    {
        return false;
    }

    *outValue = (uint32_t)number;
    return true;
}

typedef struct
{
    char **items;
    int count;
} TokenList;

static void freeTokenList(TokenList tokens)
{
    int i = 0;

    for (i = 0; i < tokens.count; i++)
    {
        free(tokens.items[i]);
    }
    free(tokens.items);
}

static TokenList splitTokens(const char *line)
{
    TokenList tokens;
    size_t capacity = 8;
    const char *p = line;

    tokens.items = (char **)malloc(sizeof(char *) * capacity);
    if (tokens.items == NULL)
    {
        failBuild("out of memory");
    }
    tokens.count = 0;

    while (*p != '\0')
    {
        while (*p != '\0' && (isspace((unsigned char)*p) || *p == ','))
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        {
            const char *start = p;
            size_t length = 0;
            char *token = NULL;

            while (*p != '\0' && !isspace((unsigned char)*p) && *p != ',')
            {
                p++;
            }

            length = (size_t)(p - start);
            token = (char *)malloc(length + 1);
            if (token == NULL)
            {
                failBuild("out of memory");
            }

            memcpy(token, start, length);
            token[length] = '\0';

            if ((size_t)tokens.count == capacity)
            {
                char **bigger = NULL;
                capacity *= 2;
                bigger = (char **)realloc(tokens.items, sizeof(char *) * capacity);
                if (bigger == NULL)
                {
                    failBuild("out of memory");
                }
                tokens.items = bigger;
            }

            tokens.items[tokens.count] = token;
            tokens.count++;
        }
    }

    return tokens;
}

static int countCharCommas(const char *text)
{
    int count = 0;
    const char *p = text;

    while (p != NULL && *p != '\0')
    {
        if (*p == ',')
        {
            count++;
        }
        p++;
    }

    return count;
}

static int expectedOperandCommaCount(const char *mnemonic)
{
    if (mnemonic == NULL)
    {
        return -1;
    }

    if (strcmp(mnemonic, "") == 0)
    {
        return 0;
    }
    if (strcmp(mnemonic, "halt") == 0)
    {
        return 0;
    }

    if (strcmp(mnemonic, "br") == 0)
    {
        return 0;
    }
    if (strcmp(mnemonic, "brr") == 0)
    {
        return 0;
    }
    if (strcmp(mnemonic, "call") == 0)
    {
        return 0;
    }
    if (strcmp(mnemonic, "return") == 0)
    {
        return 0;
    }

    if (strcmp(mnemonic, "not") == 0)
    {
        return 1;
    }
    if (strcmp(mnemonic, "addi") == 0)
    {
        return 1;
    }
    if (strcmp(mnemonic, "subi") == 0)
    {
        return 1;
    }
    if (strcmp(mnemonic, "shftri") == 0)
    {
        return 1;
    }
    if (strcmp(mnemonic, "shftli") == 0)
    {
        return 1;
    }

    if (strcmp(mnemonic, "brnz") == 0)
    {
        return 1;
    }
    if (strcmp(mnemonic, "mov") == 0)
    {
        return 1;
    }
    if (strcmp(mnemonic, "in") == 0)
    {
        return 1;
    }
    if (strcmp(mnemonic, "out") == 0)
    {
        return 1;
    }

    if (strcmp(mnemonic, "clr") == 0)
    {
        return 0;
    }
    if (strcmp(mnemonic, "push") == 0)
    {
        return 0;
    }
    if (strcmp(mnemonic, "pop") == 0)
    {
        return 0;
    }
    if (strcmp(mnemonic, "ld") == 0)
    {
        return 1;
    }

    if (strcmp(mnemonic, "priv") == 0)
    {
        return 3;
    }

    return 2;
}

static void enforceCommaStyle(const char *rawLine, const char *mnemonic)
{
    int expected = expectedOperandCommaCount(mnemonic);
    int actual = 0;

    if (expected < 0)
    {
        return;
    }

    actual = countCharCommas(rawLine);
    if (actual != expected)
    {
        failBuild("malformed operand separators");
    }
}

typedef enum
{
    partNone,
    partCode,
    partData
} AssemblyPart;

typedef enum
{
    recordInstruction,
    recordData,
    recordLoadLabel
} RecordType;

typedef struct
{
    RecordType type;
    uint64_t address;
    char *text;
    uint64_t data;
    int destReg;
} ProgramRecord;

typedef struct
{
    ProgramRecord *items;
    size_t count;
    size_t capacity;
} ProgramRecordList;

static void appendRecord(ProgramRecordList *list, ProgramRecord record)
{
    if (list->count == list->capacity)
    {
        size_t newCapacity = 0;
        ProgramRecord *bigger = NULL;

        if (list->capacity == 0)
        {
            newCapacity = 64;
        }
        else
        {
            newCapacity = list->capacity * 2;
        }

        bigger = (ProgramRecord *)realloc(list->items, newCapacity * sizeof(ProgramRecord));
        if (bigger == NULL)
        {
            failBuild("out of memory");
        }

        list->items = bigger;
        list->capacity = newCapacity;
    }

    list->items[list->count] = record;
    list->count++;
}

static void freeRecordList(ProgramRecordList *list)
{
    size_t i = 0;

    for (i = 0; i < list->count; i++)
    {
        free(list->items[i].text);
    }

    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

typedef struct
{
    char *name;
    uint64_t address;
} Symbol;

typedef struct
{
    Symbol *items;
    size_t count;
    size_t capacity;
} SymbolTable;

static void addSymbol(SymbolTable *table, const char *name, uint64_t address)
{
    size_t i = 0;

    for (i = 0; i < table->count; i++)
    {
        if (strcmp(table->items[i].name, name) == 0)
        {
            failBuildWithName("duplicate label %s", name);
        }
    }

    if (table->count == table->capacity)
    {
        size_t newCapacity = 0;
        Symbol *bigger = NULL;

        if (table->capacity == 0)
        {
            newCapacity = 64;
        }
        else
        {
            newCapacity = table->capacity * 2;
        }

        bigger = (Symbol *)realloc(table->items, newCapacity * sizeof(Symbol));
        if (bigger == NULL)
        {
            failBuild("out of memory");
        }

        table->items = bigger;
        table->capacity = newCapacity;
    }

    table->items[table->count].name = duplicateText(name);
    table->items[table->count].address = address;
    table->count++;
}

static bool findSymbol(const SymbolTable *table, const char *name, uint64_t *outAddress)
{
    size_t i = 0;

    for (i = 0; i < table->count; i++)
    {
        if (strcmp(table->items[i].name, name) == 0)
        {
            *outAddress = table->items[i].address;
            return true;
        }
    }

    return false;
}

static void freeSymbolTable(SymbolTable *table)
{
    size_t i = 0;

    for (i = 0; i < table->count; i++)
    {
        free(table->items[i].name);
    }

    free(table->items);
    table->items = NULL;
    table->count = 0;
    table->capacity = 0;
}

typedef struct
{
    char **names;
    size_t count;
    size_t capacity;
} UnattachedLabels;

static void addUnattachedLabel(UnattachedLabels *pending, const char *name)
{
    if (pending->count == pending->capacity)
    {
        size_t newCapacity = 0;
        char **bigger = NULL;

        if (pending->capacity == 0)
        {
            newCapacity = 32;
        }
        else
        {
            newCapacity = pending->capacity * 2;
        }

        bigger = (char **)realloc(pending->names, newCapacity * sizeof(char *));
        if (bigger == NULL)
        {
            failBuild("out of memory");
        }

        pending->names = bigger;
        pending->capacity = newCapacity;
    }

    pending->names[pending->count] = duplicateText(name);
    pending->count++;
}

static void attachPendingLabels(UnattachedLabels *pending, SymbolTable *symbols, uint64_t address)
{
    size_t i = 0;

    for (i = 0; i < pending->count; i++)
    {
        addSymbol(symbols, pending->names[i], address);
        free(pending->names[i]);
    }

    pending->count = 0;
}

static void freeUnattachedLabels(UnattachedLabels *pending)
{
    size_t i = 0;

    for (i = 0; i < pending->count; i++)
    {
        free(pending->names[i]);
    }

    free(pending->names);
    pending->names = NULL;
    pending->count = 0;
    pending->capacity = 0;
}

static char *readLabelDefinition(const char *line)
{
    const char *p = line;
    const char *start = NULL;

    if (p == NULL || (p[0] != '@' && p[0] != ':'))
    {
        failBuild("malformed label token");
    }

    p++;

    if (*p == '\0' || isspace((unsigned char)*p))
    {
        failBuild("malformed label token");
    }

    if (!isalpha((unsigned char)*p) && *p != '_' && *p != '.')
    {
        failBuild("malformed label token");
    }

    start = p;

    while (*p != '\0')
    {
        if (isalnum((unsigned char)*p) || *p == '_' || *p == '.')
        {
            p++;
        }
        else
        {
            failBuild("malformed label token");
        }
    }

    return duplicateTextN(start, (size_t)(p - start));
}

static void addInstructionText(ProgramRecordList *code, uint64_t address, const char *text, UnattachedLabels *pending, SymbolTable *symbols)
{
    ProgramRecord record;

    attachPendingLabels(pending, symbols, address);

    record.type = recordInstruction;
    record.address = address;
    record.text = duplicateText(text);
    record.data = 0;
    record.destReg = -1;

    appendRecord(code, record);
}

static void addLoadLabelRecord(ProgramRecordList *code, uint64_t address, int destReg, const char *labelName, UnattachedLabels *pending, SymbolTable *symbols)
{
    ProgramRecord record;

    attachPendingLabels(pending, symbols, address);

    record.type = recordLoadLabel;
    record.address = address;
    record.text = duplicateText(labelName);
    record.data = 0;
    record.destReg = destReg;

    appendRecord(code, record);
}

static void addDataValue(ProgramRecordList *data, uint64_t address, uint64_t value, UnattachedLabels *pending, SymbolTable *symbols)
{
    ProgramRecord record;

    attachPendingLabels(pending, symbols, address);

    record.type = recordData;
    record.address = address;
    record.text = NULL;
    record.data = value;
    record.destReg = -1;

    appendRecord(data, record);
}

static void addDataLabelReference(ProgramRecordList *data, uint64_t address, const char *labelName, UnattachedLabels *pending, SymbolTable *symbols)
{
    ProgramRecord record;

    attachPendingLabels(pending, symbols, address);

    record.type = recordData;
    record.address = address;
    record.text = duplicateText(labelName);
    record.data = 0;
    record.destReg = -1;

    appendRecord(data, record);
}

static void emitClearRegister(ProgramRecordList *code, uint64_t *pc, int destReg, UnattachedLabels *pending, SymbolTable *symbols)
{
    char line[64];

    snprintf(line, sizeof(line), "xor r%d, r%d, r%d", destReg, destReg, destReg);
    addInstructionText(code, *pc, line, pending, symbols);
    *pc += 4;
}

static void emitHaltInstruction(ProgramRecordList *code, uint64_t *pc, UnattachedLabels *pending, SymbolTable *symbols)
{
    addInstructionText(code, *pc, "priv r0, r0, r0, 0", pending, symbols);
    *pc += 4;
}

static void emitInputInstruction(ProgramRecordList *code, uint64_t *pc, int destReg, int srcReg, UnattachedLabels *pending, SymbolTable *symbols)
{
    char line[64];

    snprintf(line, sizeof(line), "priv r%d, r%d, r0, 3", destReg, srcReg);
    addInstructionText(code, *pc, line, pending, symbols);
    *pc += 4;
}

static void emitOutputInstruction(ProgramRecordList *code, uint64_t *pc, int destReg, int srcReg, UnattachedLabels *pending, SymbolTable *symbols)
{
    char line[64];

    snprintf(line, sizeof(line), "priv r%d, r%d, r0, 4", destReg, srcReg);
    addInstructionText(code, *pc, line, pending, symbols);
    *pc += 4;
}

static void emitPushRegister(ProgramRecordList *code, uint64_t *pc, int srcReg, UnattachedLabels *pending, SymbolTable *symbols)
{
    char line[64];

    snprintf(line, sizeof(line), "mov (r31)(-8), r%d", srcReg);
    addInstructionText(code, *pc, line, pending, symbols);
    *pc += 4;

    addInstructionText(code, *pc, "subi r31, 8", pending, symbols);
    *pc += 4;
}

static void emitPopRegister(ProgramRecordList *code, uint64_t *pc, int destReg, UnattachedLabels *pending, SymbolTable *symbols)
{
    char line[64];

    snprintf(line, sizeof(line), "mov r%d, (r31)(0)", destReg);
    addInstructionText(code, *pc, line, pending, symbols);
    *pc += 4;

    addInstructionText(code, *pc, "addi r31, 8", pending, symbols);
    *pc += 4;
}

static void emitLoadImmediate64(ProgramRecordList *code, uint64_t *pc, int destReg, uint64_t value, UnattachedLabels *pending, SymbolTable *symbols)
{
    const int shiftAmounts[5] = {12, 12, 12, 12, 4};
    const int offsets[5] = {40, 28, 16, 4, 0};
    char line[64];
    uint64_t top = 0;
    int i = 0;

    snprintf(line, sizeof(line), "xor r%d, r%d, r%d", destReg, destReg, destReg);
    addInstructionText(code, *pc, line, pending, symbols);
    *pc += 4;

    top = (value >> 52) & 0xFFFULL;
    snprintf(line, sizeof(line), "addi r%d, %llu", destReg, (unsigned long long)top);
    addInstructionText(code, *pc, line, pending, symbols);
    *pc += 4;

    for (i = 0; i < 5; i++)
    {
        uint64_t part = 0;

        snprintf(line, sizeof(line), "shftli r%d, %d", destReg, shiftAmounts[i]);
        addInstructionText(code, *pc, line, pending, symbols);
        *pc += 4;

        if (i == 4)
        {
            part = value & 0xFULL;
        }
        else
        {
            part = (value >> (uint64_t)offsets[i]) & 0xFFFULL;
        }

        snprintf(line, sizeof(line), "addi r%d, %llu", destReg, (unsigned long long)part);
        addInstructionText(code, *pc, line, pending, symbols);
        *pc += 4;
    }
}

static uint32_t encodeRType(uint32_t opcode, uint32_t rd, uint32_t rs, uint32_t rt)
{
    uint32_t word = 0;

    word |= (opcode & 0x1Fu) << 27;
    word |= (rd & 0x1Fu) << 22;
    word |= (rs & 0x1Fu) << 17;
    word |= (rt & 0x1Fu) << 12;

    return word;
}

static uint32_t encodeIType(uint32_t opcode, uint32_t rd, uint32_t rs, uint32_t imm12)
{
    uint32_t word = 0;

    word |= (opcode & 0x1Fu) << 27;
    word |= (rd & 0x1Fu) << 22;
    word |= (rs & 0x1Fu) << 17;
    word |= (imm12 & 0xFFFu);

    return word;
}

static uint32_t encodePType(uint32_t opcode, uint32_t rd, uint32_t rs, uint32_t rt, uint32_t imm12)
{
    uint32_t word = 0;

    word |= (opcode & 0x1Fu) << 27;
    word |= (rd & 0x1Fu) << 22;
    word |= (rs & 0x1Fu) << 17;
    word |= (rt & 0x1Fu) << 12;
    word |= (imm12 & 0xFFFu);

    return word;
}

static bool readMemoryOperandParen(const char *token, int *outBaseReg, int32_t *outSignedImm)
{
    const char *p = NULL;
    const char *close1 = NULL;
    const char *close2 = NULL;
    size_t len1 = 0;
    size_t len2 = 0;
    char regText[40];
    char immText[80];
    int baseReg = -1;
    int32_t signedImm = 0;

    if (token == NULL || token[0] != '(')
    {
        return false;
    }

    p = token + 1;
    close1 = strchr(p, ')');
    if (close1 == NULL)
    {
        return false;
    }

    len1 = (size_t)(close1 - p);
    if (len1 == 0 || len1 >= 32)
    {
        return false;
    }

    memcpy(regText, p, len1);
    regText[len1] = '\0';

    p = close1 + 1;
    if (*p != '(')
    {
        return false;
    }
    p++;

    close2 = strchr(p, ')');
    if (close2 == NULL)
    {
        return false;
    }

    len2 = (size_t)(close2 - p);
    if (len2 == 0 || len2 >= 64)
    {
        return false;
    }

    memcpy(immText, p, len2);
    immText[len2] = '\0';

    if (close2[1] != '\0')
    {
        return false;
    }

    baseReg = readRegisterNumber(regText);
    if (baseReg < 0)
    {
        return false;
    }

    if (!readSigned12(immText, &signedImm))
    {
        return false;
    }

    *outBaseReg = baseReg;
    *outSignedImm = signedImm;
    return true;
}

static uint32_t assembleOneInstruction(const char *instructionText, uint64_t pc, const SymbolTable *symbols)
{
    TokenList tokens;
    const char *mnemonic = NULL;

    tokens = splitTokens(instructionText);

    if (tokens.count == 0)
    {
        freeTokenList(tokens);
        failBuild("empty instruction");
    }

    {
        char *c = tokens.items[0];
        while (*c != '\0')
        {
            *c = (char)tolower((unsigned char)*c);
            c++;
        }
    }

    mnemonic = tokens.items[0];

    if (strcmp(mnemonic, "and") == 0 || strcmp(mnemonic, "or") == 0 || strcmp(mnemonic, "xor") == 0 ||
        strcmp(mnemonic, "add") == 0 || strcmp(mnemonic, "sub") == 0 || strcmp(mnemonic, "mul") == 0 || strcmp(mnemonic, "div") == 0 ||
        strcmp(mnemonic, "addf") == 0 || strcmp(mnemonic, "subf") == 0 || strcmp(mnemonic, "mulf") == 0 || strcmp(mnemonic, "divf") == 0 ||
        strcmp(mnemonic, "shftr") == 0 || strcmp(mnemonic, "shftl") == 0)
    {
        int rd = -1;
        int rs = -1;
        int rt = -1;
        uint32_t opcode = 0;

        if (tokens.count != 4)
        {
            freeTokenList(tokens);
            failBuild("R-type expects 3 registers");
        }

        rd = readRegisterNumber(tokens.items[1]);
        rs = readRegisterNumber(tokens.items[2]);
        rt = readRegisterNumber(tokens.items[3]);

        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        if (strcmp(mnemonic, "and") == 0)
        {
            opcode = 0x00;
        }
        else if (strcmp(mnemonic, "or") == 0)
        {
            opcode = 0x01;
        }
        else if (strcmp(mnemonic, "xor") == 0)
        {
            opcode = 0x02;
        }
        else if (strcmp(mnemonic, "shftr") == 0)
        {
            opcode = 0x04;
        }
        else if (strcmp(mnemonic, "shftl") == 0)
        {
            opcode = 0x06;
        }
        else if (strcmp(mnemonic, "addf") == 0)
        {
            opcode = 0x14;
        }
        else if (strcmp(mnemonic, "subf") == 0)
        {
            opcode = 0x15;
        }
        else if (strcmp(mnemonic, "mulf") == 0)
        {
            opcode = 0x16;
        }
        else if (strcmp(mnemonic, "divf") == 0)
        {
            opcode = 0x17;
        }
        else if (strcmp(mnemonic, "add") == 0)
        {
            opcode = 0x18;
        }
        else if (strcmp(mnemonic, "sub") == 0)
        {
            opcode = 0x1A;
        }
        else if (strcmp(mnemonic, "mul") == 0)
        {
            opcode = 0x1C;
        }
        else if (strcmp(mnemonic, "div") == 0)
        {
            opcode = 0x1D;
        }
        else
        {
            freeTokenList(tokens);
            failBuild("unknown instruction");
        }

        freeTokenList(tokens);
        return encodeRType(opcode, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt);
    }

    if (strcmp(mnemonic, "not") == 0)
    {
        int rd = -1;
        int rs = -1;

        if (tokens.count != 3)
        {
            freeTokenList(tokens);
            failBuild("not expects 2 registers");
        }

        rd = readRegisterNumber(tokens.items[1]);
        rs = readRegisterNumber(tokens.items[2]);

        if (rd < 0 || rs < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        freeTokenList(tokens);
        return encodeRType(0x03, (uint32_t)rd, (uint32_t)rs, 0);
    }

    if (strcmp(mnemonic, "addi") == 0 || strcmp(mnemonic, "subi") == 0 || strcmp(mnemonic, "shftri") == 0 || strcmp(mnemonic, "shftli") == 0)
    {
        int rd = -1;
        uint32_t imm = 0;
        uint32_t opcode = 0;

        if (tokens.count != 3)
        {
            freeTokenList(tokens);
            failBuild("I-type expects rd, imm");
        }

        rd = readRegisterNumber(tokens.items[1]);
        if (rd < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        if (!readUnsigned12(tokens.items[2], &imm))
        {
            freeTokenList(tokens);
            failBuild("immediate must be 0..4095");
        }

        if (strcmp(mnemonic, "addi") == 0)
        {
            opcode = 0x19;
        }
        else if (strcmp(mnemonic, "subi") == 0)
        {
            opcode = 0x1B;
        }
        else if (strcmp(mnemonic, "shftri") == 0)
        {
            opcode = 0x05;
        }
        else
        {
            opcode = 0x07;
        }

        freeTokenList(tokens);
        return encodeIType(opcode, (uint32_t)rd, 0, imm);
    }

    if (strcmp(mnemonic, "br") == 0)
    {
        int rd = -1;

        if (tokens.count != 2)
        {
            freeTokenList(tokens);
            failBuild("br expects rd");
        }

        rd = readRegisterNumber(tokens.items[1]);
        if (rd < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        freeTokenList(tokens);
        return encodeRType(0x08, (uint32_t)rd, 0, 0);
    }

    if (strcmp(mnemonic, "brr") == 0)
    {
        if (tokens.count != 2)
        {
            freeTokenList(tokens);
            failBuild("brr expects rd or imm/label");
        }

        {
            int reg = readRegisterNumber(tokens.items[1]);
            if (reg >= 0)
            {
                freeTokenList(tokens);
                return encodeRType(0x09, (uint32_t)reg, 0, 0);
            }
        }

        if (tokens.items[1][0] == ':' || tokens.items[1][0] == '@')
        {
            uint64_t target = 0;
            const char *name = tokens.items[1] + 1;
            int64_t delta = 0;
            uint32_t imm12 = 0;

            if (!findSymbol(symbols, name, &target))
            {
                freeTokenList(tokens);
                failBuildWithName("undefined label reference %s", tokens.items[1]);
            }

            delta = (int64_t)target - (int64_t)pc;

            if (delta < -2048LL || delta > 2047LL)
            {
                freeTokenList(tokens);
                failBuild("brr label out of range for signed 12-bit");
            }

            imm12 = (uint32_t)((int32_t)delta) & 0xFFFu;
            freeTokenList(tokens);
            return ((0x0Au & 0x1Fu) << 27) | imm12;
        }

        {
            int32_t rel = 0;
            uint32_t imm12 = 0;

            if (!readSigned12(tokens.items[1], &rel))
            {
                freeTokenList(tokens);
                failBuild("brr immediate must fit signed 12-bit");
            }

            imm12 = (uint32_t)rel & 0xFFFu;
            freeTokenList(tokens);
            return ((0x0Au & 0x1Fu) << 27) | imm12;
        }
    }

    if (strcmp(mnemonic, "brnz") == 0)
    {
        int rd = -1;
        int rs = -1;

        if (tokens.count != 3)
        {
            freeTokenList(tokens);
            failBuild("brnz expects rd, rs");
        }

        rd = readRegisterNumber(tokens.items[1]);
        rs = readRegisterNumber(tokens.items[2]);

        if (rd < 0 || rs < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        freeTokenList(tokens);
        return encodeRType(0x0B, (uint32_t)rd, (uint32_t)rs, 0);
    }

    if (strcmp(mnemonic, "call") == 0)
    {
        int rd = -1;

        if (tokens.count != 2)
        {
            freeTokenList(tokens);
            failBuild("call expects rd");
        }

        rd = readRegisterNumber(tokens.items[1]);
        if (rd < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        freeTokenList(tokens);
        return encodeRType(0x0C, (uint32_t)rd, 0, 0);
    }

    if (strcmp(mnemonic, "return") == 0)
    {
        if (tokens.count != 1)
        {
            freeTokenList(tokens);
            failBuild("return expects no operands");
        }

        freeTokenList(tokens);
        return (0x0Du & 0x1Fu) << 27;
    }

    if (strcmp(mnemonic, "brgt") == 0)
    {
        int rd = -1;
        int rs = -1;
        int rt = -1;

        if (tokens.count != 4)
        {
            freeTokenList(tokens);
            failBuild("brgt expects rd, rs, rt");
        }

        rd = readRegisterNumber(tokens.items[1]);
        rs = readRegisterNumber(tokens.items[2]);
        rt = readRegisterNumber(tokens.items[3]);

        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        freeTokenList(tokens);
        return encodeRType(0x0E, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt);
    }

    if (strcmp(mnemonic, "priv") == 0)
    {
        int rd = -1;
        int rs = -1;
        int rt = -1;
        uint32_t imm = 0;

        if (tokens.count != 5)
        {
            freeTokenList(tokens);
            failBuild("priv expects rd, rs, rt, imm");
        }

        rd = readRegisterNumber(tokens.items[1]);
        rs = readRegisterNumber(tokens.items[2]);
        rt = readRegisterNumber(tokens.items[3]);

        if (rd < 0 || rs < 0 || rt < 0)
        {
            freeTokenList(tokens);
            failBuild("invalid register");
        }

        if (!readUnsigned12(tokens.items[4], &imm))
        {
            freeTokenList(tokens);
            failBuild("priv imm must be 0..4095");
        }

        freeTokenList(tokens);
        return encodePType(0x0F, (uint32_t)rd, (uint32_t)rs, (uint32_t)rt, imm);
    }

    if (strcmp(mnemonic, "mov") == 0)
    {
        const char *left = NULL;
        const char *right = NULL;

        if (tokens.count != 3)
        {
            freeTokenList(tokens);
            failBuild("mov expects 2 operands");
        }

        left = tokens.items[1];
        right = tokens.items[2];

        if ((left[0] == 'r' || left[0] == 'R') && (strchr(left, '+') != NULL || strchr(left, '-') != NULL))
        {
            freeTokenList(tokens);
            failBuild("mov malformed memory operand");
        }

        if ((right[0] == 'r' || right[0] == 'R') && (strchr(right, '+') != NULL || strchr(right, '-') != NULL))
        {
            freeTokenList(tokens);
            failBuild("mov malformed memory operand");
        }

        if (left[0] == '(')
        {
            int base = -1;
            int32_t signedImm = 0;
            int src = -1;

            if (!readMemoryOperandParen(left, &base, &signedImm))
            {
                freeTokenList(tokens);
                failBuild("mov store malformed memory operand");
            }

            src = readRegisterNumber(right);
            if (src < 0)
            {
                freeTokenList(tokens);
                failBuild("mov store invalid source reg");
            }

            freeTokenList(tokens);
            return encodePType(0x13, (uint32_t)base, (uint32_t)src, 0, (uint32_t)signedImm & 0xFFFu);
        }

        if (right[0] == '(')
        {
            int dst = -1;
            int base = -1;
            int32_t signedImm = 0;

            dst = readRegisterNumber(left);
            if (dst < 0)
            {
                freeTokenList(tokens);
                failBuild("mov load invalid rd");
            }

            if (!readMemoryOperandParen(right, &base, &signedImm))
            {
                freeTokenList(tokens);
                failBuild("mov load malformed memory operand");
            }

            freeTokenList(tokens);
            return encodePType(0x10, (uint32_t)dst, (uint32_t)base, 0, (uint32_t)signedImm & 0xFFFu);
        }

        {
            int dst = -1;
            int src = -1;
            uint32_t imm = 0;

            dst = readRegisterNumber(left);
            if (dst < 0)
            {
                freeTokenList(tokens);
                failBuild("mov invalid rd");
            }

            src = readRegisterNumber(right);
            if (src >= 0)
            {
                freeTokenList(tokens);
                return encodeRType(0x11, (uint32_t)dst, (uint32_t)src, 0);
            }

            if (!readUnsigned12(right, &imm))
            {
                freeTokenList(tokens);
                failBuild("mov rd, L: L must be 0..4095");
            }

            freeTokenList(tokens);
            return encodeIType(0x12, (uint32_t)dst, 0, imm);
        }
    }

    {
        char *mnCopy = duplicateText(mnemonic);
        freeTokenList(tokens);
        failBuildWithName("unknown instruction mnemonic %s", mnCopy);
    }

    return 0;
}

static void expandLoadLabelRecords(ProgramRecordList *code, const SymbolTable *symbols)
{
    ProgramRecordList expanded;
    size_t i = 0;

    expanded.items = NULL;
    expanded.count = 0;
    expanded.capacity = 0;

    for (i = 0; i < code->count; i++)
    {
        ProgramRecord record = code->items[i];

        if (record.type != recordLoadLabel)
        {
            appendRecord(&expanded, record);
            code->items[i].text = NULL;
        }
        else
        {
            uint64_t target = 0;
            uint64_t localPc = 0;
            UnattachedLabels tempPending;
            SymbolTable tempSymbols;

            if (!findSymbol(symbols, record.text, &target))
            {
                failBuildWithName("ld: undefined label reference %s", record.text);
            }

            localPc = record.address;

            tempPending.names = NULL;
            tempPending.count = 0;
            tempPending.capacity = 0;

            tempSymbols.items = symbols->items;
            tempSymbols.count = symbols->count;
            tempSymbols.capacity = symbols->capacity;

            emitLoadImmediate64(&expanded, &localPc, record.destReg, target, &tempPending, &tempSymbols);

            freeUnattachedLabels(&tempPending);
            free(record.text);
        }
    }

    free(code->items);
    *code = expanded;
}

static void buildFromSource(const char *inputPath, ProgramRecordList *code, ProgramRecordList *data, SymbolTable *symbols)
{
    FILE *file = NULL;
    char rawLine[4096];
    AssemblyPart currentPart = partNone;
    uint64_t codePc = programCodeBase;
    uint64_t dataPc = programDataBase;
    UnattachedLabels pendingLabels;
    bool sawCodeDirective = false;

    file = fopen(inputPath, "r");
    if (file == NULL)
    {
        failBuildWithName("cannot open input file %s", inputPath);
    }

    pendingLabels.names = NULL;
    pendingLabels.count = 0;
    pendingLabels.capacity = 0;

    while (fgets(rawLine, sizeof(rawLine), file) != NULL)
    {
        const char *p = NULL;

        rstripWhitespace(rawLine);
        stripSemicolonComment(rawLine);
        rstripWhitespace(rawLine);

        p = rawLine;

        if (*p == '\0')
        {
            continue;
        }

        if (hasPrefix(p, ".code"))
        {
            currentPart = partCode;
            sawCodeDirective = true;
            continue;
        }

        if (hasPrefix(p, ".data"))
        {
            currentPart = partData;
            continue;
        }

        if (p[0] == ':' || p[0] == '@')
        {
            char *name = readLabelDefinition(p);
            addUnattachedLabel(&pendingLabels, name);
            free(name);
            continue;
        }

        if (p[0] != '\t')
        {
            fclose(file);
            freeUnattachedLabels(&pendingLabels);
            failBuild("code/data line must start with tab character");
        }

        p = skipLeadingWhitespace(p);

        if (*p == '\0')
        {
            continue;
        }

        if (currentPart == partNone)
        {
            fclose(file);
            freeUnattachedLabels(&pendingLabels);
            failBuild("code/data line before any .code or .data directive");
        }

        if (currentPart == partData)
        {
            if ((p[0] == ':' || p[0] == '@') && p[1] != '\0')
            {
                addDataLabelReference(data, dataPc, p + 1, &pendingLabels, symbols);
                dataPc += 8;
                continue;
            }

            {
                uint64_t value = 0;
                if (!readUnsigned64(p, &value))
                {
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("malformed data item; expected 64-bit unsigned integer");
                }

                addDataValue(data, dataPc, value, &pendingLabels, symbols);
                dataPc += 8;
                continue;
            }
        }

        {
            TokenList tokens = splitTokens(p);
            char mnemonic[64];
            int i = 0;

            if (tokens.count == 0)
            {
                freeTokenList(tokens);
                continue;
            }

            for (i = 0; tokens.items[0][i] != '\0'; i++)
            {
                tokens.items[0][i] = (char)tolower((unsigned char)tokens.items[0][i]);
            }

            snprintf(mnemonic, sizeof(mnemonic), "%s", tokens.items[0]);

            enforceCommaStyle(p, mnemonic);
            freeTokenList(tokens);

            if (strcmp(mnemonic, "clr") == 0)
            {
                TokenList t = splitTokens(p);
                int rd = -1;

                if (t.count != 2)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("clr expects clr rd");
                }

                rd = readRegisterNumber(t.items[1]);
                if (rd < 0)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("clr invalid register");
                }

                freeTokenList(t);
                emitClearRegister(code, &codePc, rd, &pendingLabels, symbols);
                continue;
            }

            if (strcmp(mnemonic, "halt") == 0)
            {
                TokenList t = splitTokens(p);

                if (t.count != 1)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("halt expects no operands");
                }

                freeTokenList(t);
                emitHaltInstruction(code, &codePc, &pendingLabels, symbols);
                continue;
            }

            if (strcmp(mnemonic, "in") == 0)
            {
                TokenList t = splitTokens(p);
                int rd = -1;
                int rs = -1;

                if (t.count != 3)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("in expects in rd, rs");
                }

                rd = readRegisterNumber(t.items[1]);
                rs = readRegisterNumber(t.items[2]);

                if (rd < 0 || rs < 0)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("in invalid register");
                }

                freeTokenList(t);
                emitInputInstruction(code, &codePc, rd, rs, &pendingLabels, symbols);
                continue;
            }

            if (strcmp(mnemonic, "out") == 0)
            {
                TokenList t = splitTokens(p);
                int rd = -1;
                int rs = -1;

                if (t.count != 3)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("out expects out rd, rs");
                }

                rd = readRegisterNumber(t.items[1]);
                rs = readRegisterNumber(t.items[2]);

                if (rd < 0 || rs < 0)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("out invalid register");
                }

                freeTokenList(t);
                emitOutputInstruction(code, &codePc, rd, rs, &pendingLabels, symbols);
                continue;
            }

            if (strcmp(mnemonic, "push") == 0)
            {
                TokenList t = splitTokens(p);
                int rd = -1;

                if (t.count != 2)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("push expects push rd");
                }

                rd = readRegisterNumber(t.items[1]);
                if (rd < 0)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("push invalid register");
                }

                freeTokenList(t);
                emitPushRegister(code, &codePc, rd, &pendingLabels, symbols);
                continue;
            }

            if (strcmp(mnemonic, "pop") == 0)
            {
                TokenList t = splitTokens(p);
                int rd = -1;

                if (t.count != 2)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("pop expects pop rd");
                }

                rd = readRegisterNumber(t.items[1]);
                if (rd < 0)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("pop invalid register");
                }

                freeTokenList(t);
                emitPopRegister(code, &codePc, rd, &pendingLabels, symbols);
                continue;
            }

            if (strcmp(mnemonic, "ld") == 0)
            {
                TokenList t = splitTokens(p);
                int rd = -1;

                if (t.count != 3)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("ld expects ld rd, valueOrLabel");
                }

                rd = readRegisterNumber(t.items[1]);
                if (rd < 0)
                {
                    freeTokenList(t);
                    fclose(file);
                    freeUnattachedLabels(&pendingLabels);
                    failBuild("ld invalid register");
                }

                if ((t.items[2][0] == ':' || t.items[2][0] == '@') && t.items[2][1] != '\0')
                {
                    addLoadLabelRecord(code, codePc, rd, t.items[2] + 1, &pendingLabels, symbols);
                    codePc += 48;
                    freeTokenList(t);
                    continue;
                }

                {
                    uint64_t imm = 0;
                    if (!readUnsigned64(t.items[2], &imm))
                    {
                        freeTokenList(t);
                        fclose(file);
                        freeUnattachedLabels(&pendingLabels);
                        failBuild("ld invalid literal");
                    }

                    freeTokenList(t);
                    emitLoadImmediate64(code, &codePc, rd, imm, &pendingLabels, symbols);
                    continue;
                }
            }

            addInstructionText(code, codePc, p, &pendingLabels, symbols);
            codePc += 4;
        }
    }

    fclose(file);

    if (pendingLabels.count != 0)
    {
        freeUnattachedLabels(&pendingLabels);
        failBuild("label at end of file without following instruction/data");
    }

    freeUnattachedLabels(&pendingLabels);

    if (!sawCodeDirective)
    {
        failBuild("program must have at least one .code directive");
    }

    expandLoadLabelRecords(code, symbols);
}

static uint32_t *assembleProgramWords(const ProgramRecordList *code, const SymbolTable *symbols)
{
    uint32_t *words = NULL;
    size_t i = 0;

    words = (uint32_t *)malloc(sizeof(uint32_t) * code->count);
    if (words == NULL)
    {
        failBuild("out of memory");
    }

    for (i = 0; i < code->count; i++)
    {
        if (code->items[i].type != recordInstruction)
        {
            failBuild("internal error: non-instruction in code list");
        }
        words[i] = assembleOneInstruction(code->items[i].text, code->items[i].address, symbols);
    }

    return words;
}

static void writeOutputTko(const char *outputPath, const ProgramRecordList *code, const ProgramRecordList *data, const uint32_t *words, const SymbolTable *symbols)
{
    FILE *file = NULL;
    uint64_t fileType = 0ULL;
    uint64_t codeBegin = programCodeBase;
    uint64_t codeSize = (uint64_t)code->count * 4ULL;
    uint64_t dataBegin = programDataBase;
    uint64_t dataSize = (uint64_t)data->count * 8ULL;
    size_t i = 0;

    file = fopen(outputPath, "wb");
    if (file == NULL)
    {
        failBuildWithName("cannot open output file %s", outputPath);
    }

    writeU64LittleEndian(file, fileType);
    writeU64LittleEndian(file, codeBegin);
    writeU64LittleEndian(file, codeSize);
    writeU64LittleEndian(file, dataBegin);
    writeU64LittleEndian(file, dataSize);

    for (i = 0; i < code->count; i++)
    {
        writeU32LittleEndian(file, words[i]);
    }

    for (i = 0; i < data->count; i++)
    {
        if (data->items[i].text != NULL)
        {
            uint64_t address = 0;

            if (!findSymbol(symbols, data->items[i].text, &address))
            {
                fclose(file);
                failBuildWithName("undefined label reference %s", data->items[i].text);
            }

            writeU64LittleEndian(file, address);
        }
        else
        {
            writeU64LittleEndian(file, data->items[i].data);
        }
    }

    fclose(file);
}

int main(int argc, char **argv)
{
    const char *inputPath = NULL;
    const char *outputPath = NULL;

    ProgramRecordList code;
    ProgramRecordList data;
    SymbolTable symbols;

    uint32_t *words = NULL;

    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s input.tk output.tko\n", argv[0]);
        return 1;
    }

    inputPath = argv[1];
    outputPath = argv[2];

    code.items = NULL;
    code.count = 0;
    code.capacity = 0;

    data.items = NULL;
    data.count = 0;
    data.capacity = 0;

    symbols.items = NULL;
    symbols.count = 0;
    symbols.capacity = 0;

    buildFromSource(inputPath, &code, &data, &symbols);

    words = assembleProgramWords(&code, &symbols);
    writeOutputTko(outputPath, &code, &data, words, &symbols);

    free(words);
    freeRecordList(&code);
    freeRecordList(&data);
    freeSymbolTable(&symbols);

    return 0;
}