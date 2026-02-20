#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

static const uint64_t memoryBytes = 512ULL * 1024ULL;
static const uint64_t expectedCodeBase = 0x2000ULL;
static const uint64_t expectedDataBase = 0x10000ULL;

typedef struct
{
    uint8_t *memory;
    uint64_t regs[32];
    uint64_t pc;
    bool stopped;
} Machine;

typedef void (*OpFn)(Machine *, uint32_t);

static void stopBadPath(void)
{
    fprintf(stderr, "Invalid tinker filepath\n");
    exit(1);
}

static void stopSimError(void)
{
    fprintf(stderr, "Simulation error\n");
    exit(1);
}

static uint64_t checkedAddress(int64_t signedAddress, uint64_t bytesNeeded)
{
    uint64_t address;

    if (signedAddress < 0)
    {
        stopSimError();
    }

    address = (uint64_t)signedAddress;

    if (address + bytesNeeded > memoryBytes)
    {
        stopSimError();
    }

    return address;
}

static uint32_t readU32LE(Machine *m, uint64_t address)
{
    uint32_t b0;
    uint32_t b1;
    uint32_t b2;
    uint32_t b3;
    uint32_t value;

    b0 = (uint32_t)m->memory[address + 0];
    b1 = (uint32_t)m->memory[address + 1];
    b2 = (uint32_t)m->memory[address + 2];
    b3 = (uint32_t)m->memory[address + 3];

    value = 0;
    value |= b0;
    value |= (b1 << 8);
    value |= (b2 << 16);
    value |= (b3 << 24);

    return value;
}

static uint64_t readU64LE(Machine *m, uint64_t address)
{
    uint64_t value;
    int i;

    value = 0;
    i = 0;
    while (i < 8)
    {
        uint64_t byteValue;
        uint64_t shift;

        byteValue = (uint64_t)m->memory[address + (uint64_t)i];
        shift = (uint64_t)(8 * i);

        value |= (byteValue << shift);

        i++;
    }

    return value;
}

static void writeU64LE(Machine *m, uint64_t address, uint64_t value)
{
    int i;

    i = 0;
    while (i < 8)
    {
        uint64_t shift;
        uint64_t oneByte;

        shift = (uint64_t)(8 * i);
        oneByte = (value >> shift) & 0xFFULL;

        m->memory[address + (uint64_t)i] = (uint8_t)oneByte;

        i++;
    }
}

static int64_t signExtend12(uint32_t imm12)
{
    uint32_t masked;
    int64_t result;

    masked = imm12 & 0xFFFu;

    if ((masked & 0x800u) != 0u)
    {
        uint32_t extended;
        extended = masked | 0xFFFFF000u;
        result = (int64_t)(int32_t)extended;
        return result;
    }

    result = (int64_t)(int32_t)masked;
    return result;
}

static uint32_t opcodeOf(uint32_t inst)
{
    uint32_t v;
    v = (inst >> 27) & 0x1Fu;
    return v;
}

static uint32_t rdOf(uint32_t inst)
{
    uint32_t v;
    v = (inst >> 22) & 0x1Fu;
    return v;
}

static uint32_t rsOf(uint32_t inst)
{
    uint32_t v;
    v = (inst >> 17) & 0x1Fu;
    return v;
}

static uint32_t rtOf(uint32_t inst)
{
    uint32_t v;
    v = (inst >> 12) & 0x1Fu;
    return v;
}

static uint32_t imm12Of(uint32_t inst)
{
    uint32_t v;
    v = inst & 0xFFFu;
    return v;
}

static double bitsToDouble(uint64_t bits)
{
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static uint64_t doubleToBits(double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static uint64_t readU64FromStdinStrict(void)
{
    char buf[256];
    char *end;
    unsigned long long parsed;
    uint64_t out;

    if (scanf("%255s", buf) != 1)
    {
        stopSimError();
    }

    if (buf[0] == '-' || buf[0] == '+')
    {
        stopSimError();
    }

    errno = 0;
    end = NULL;
    parsed = strtoull(buf, &end, 10);

    if (errno != 0)
    {
        stopSimError();
    }

    if (end == NULL)
    {
        stopSimError();
    }

    if (*end != '\0')
    {
        stopSimError();
    }

    out = (uint64_t)parsed;
    return out;
}

static uint64_t readU64LEFromFile(FILE *f)
{
    uint8_t b[8];
    size_t got;
    uint64_t value;
    int i;

    got = fread(b, 1, 8, f);
    if (got != 8)
    {
        stopBadPath();
    }

    value = 0;
    i = 0;
    while (i < 8)
    {
        value |= ((uint64_t)b[i]) << (uint64_t)(8 * i);
        i++;
    }

    return value;
}

static void readExactly(FILE *f, uint8_t *dst, uint64_t count)
{
    size_t got;

    if (count == 0)
    {
        return;
    }

    got = fread(dst, 1, (size_t)count, f);
    if (got != (size_t)count)
    {
        stopBadPath();
    }
}

static void loadTko(Machine *m, const char *path)
{
    FILE *f;
    uint64_t fileType;
    uint64_t codeBegin;
    uint64_t codeSize;
    uint64_t dataBegin;
    uint64_t dataSize;

    uint64_t codeEnd;
    uint64_t dataEnd;

    f = fopen(path, "rb");
    if (f == NULL)
    {
        stopBadPath();
    }

    fileType = readU64LEFromFile(f);
    codeBegin = readU64LEFromFile(f);
    codeSize = readU64LEFromFile(f);
    dataBegin = readU64LEFromFile(f);
    dataSize = readU64LEFromFile(f);

    if (fileType != 0ULL)
    {
        fclose(f);
        stopSimError();
    }

    if (codeBegin != expectedCodeBase)
    {
        fclose(f);
        stopSimError();
    }

    if (dataBegin != expectedDataBase)
    {
        fclose(f);
        stopSimError();
    }

    if ((codeSize % 4ULL) != 0ULL)
    {
        fclose(f);
        stopSimError();
    }

    if ((dataSize % 8ULL) != 0ULL)
    {
        fclose(f);
        stopSimError();
    }

    codeEnd = codeBegin + codeSize;
    dataEnd = dataBegin + dataSize;

    if (codeEnd > memoryBytes)
    {
        fclose(f);
        stopSimError();
    }

    if (dataEnd > memoryBytes)
    {
        fclose(f);
        stopSimError();
    }

    if (codeSize != 0ULL && dataSize != 0ULL)
    {
        if (codeBegin < dataEnd && dataBegin < codeEnd)
        {
            fclose(f);
            stopSimError();
        }
    }

    readExactly(f, m->memory + codeBegin, codeSize);
    readExactly(f, m->memory + dataBegin, dataSize);

    fclose(f);

    m->pc = codeBegin;
}

static void opIllegal(Machine *m, uint32_t inst)
{
    (void)m;
    (void)inst;
    stopSimError();
}

static void opAnd(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    m->regs[rd] = m->regs[rs] & m->regs[rt];
    m->pc = m->pc + 4;
}

static void opOr(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    m->regs[rd] = m->regs[rs] | m->regs[rt];
    m->pc = m->pc + 4;
}

static void opXor(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    m->regs[rd] = m->regs[rs] ^ m->regs[rt];
    m->pc = m->pc + 4;
}

static void opNot(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;

    rd = rdOf(inst);
    rs = rsOf(inst);

    m->regs[rd] = ~m->regs[rs];
    m->pc = m->pc + 4;
}

static void opShiftRightReg(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    uint64_t amount;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    amount = m->regs[rt] & 63ULL;
    m->regs[rd] = m->regs[rs] >> amount;
    m->pc = m->pc + 4;
}

static void opShiftRightImm(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint64_t amount;

    rd = rdOf(inst);

    amount = (uint64_t)(imm12Of(inst) & 63u);
    m->regs[rd] = m->regs[rd] >> amount;
    m->pc = m->pc + 4;
}

static void opShiftLeftReg(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    uint64_t amount;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    amount = m->regs[rt] & 63ULL;
    m->regs[rd] = m->regs[rs] << amount;
    m->pc = m->pc + 4;
}

static void opShiftLeftImm(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint64_t amount;

    rd = rdOf(inst);

    amount = (uint64_t)(imm12Of(inst) & 63u);
    m->regs[rd] = m->regs[rd] << amount;
    m->pc = m->pc + 4;
}

static void opBranchAbs(Machine *m, uint32_t inst)
{
    uint32_t rd;

    rd = rdOf(inst);
    m->pc = m->regs[rd];
}

static void opBranchRelReg(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint64_t offset;

    rd = rdOf(inst);
    offset = m->regs[rd];

    m->pc = m->pc + offset;
}

static void opBranchRelImm(Machine *m, uint32_t inst)
{
    int64_t offset;
    uint64_t newPc;

    offset = signExtend12(imm12Of(inst));
    newPc = (uint64_t)((int64_t)m->pc + offset);

    m->pc = newPc;
}

static void opBranchNotZero(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;

    rd = rdOf(inst);
    rs = rsOf(inst);

    if (m->regs[rs] == 0ULL)
    {
        m->pc = m->pc + 4;
    }
    else
    {
        m->pc = m->regs[rd];
    }
}

static void opCall(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint64_t sp;
    int64_t addrSigned;
    uint64_t addr;

    rd = rdOf(inst);
    sp = m->regs[31];

    addrSigned = (int64_t)sp - 8;
    addr = checkedAddress(addrSigned, 8);

    writeU64LE(m, addr, m->pc + 4);

    m->pc = m->regs[rd];
}

static void opReturn(Machine *m, uint32_t inst)
{
    uint64_t sp;
    int64_t addrSigned;
    uint64_t addr;
    uint64_t returnPc;

    (void)inst;

    sp = m->regs[31];

    addrSigned = (int64_t)sp - 8;
    addr = checkedAddress(addrSigned, 8);

    returnPc = readU64LE(m, addr);

    m->pc = returnPc;
}

static void opBranchGreaterThan(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t left;
    int64_t right;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    left = (int64_t)m->regs[rs];
    right = (int64_t)m->regs[rt];

    if (left > right)
    {
        m->pc = m->regs[rd];
    }
    else
    {
        m->pc = m->pc + 4;
    }
}

static void opPriv(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t imm;
    uint64_t inPort;
    uint64_t outPort;

    rd = rdOf(inst);
    rs = rsOf(inst);
    imm = imm12Of(inst);

    if (imm == 0u)
    {
        m->stopped = true;
        return;
    }

    if (imm == 3u)
    {
        inPort = m->regs[rs];

        if (inPort == 0ULL)
        {
            m->regs[rd] = readU64FromStdinStrict();
        }

        m->pc = m->pc + 4;
        return;
    }

    if (imm == 4u)
    {
        outPort = m->regs[rd];

        if (outPort == 1ULL)
        {
            printf("%llu\n", (unsigned long long)m->regs[rs]);
        }
        else if (outPort == 3ULL)
        {
            putchar((int)(m->regs[rs] & 0xFFULL));
            fflush(stdout);
        }

        m->pc = m->pc + 4;
        return;
    }

    stopSimError();
}

static void opLoad(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    int64_t offset;
    int64_t addrSigned;
    uint64_t addr;

    rd = rdOf(inst);
    rs = rsOf(inst);

    offset = signExtend12(imm12Of(inst));
    addrSigned = (int64_t)m->regs[rs] + offset;
    addr = checkedAddress(addrSigned, 8);

    m->regs[rd] = readU64LE(m, addr);
    m->pc = m->pc + 4;
}

static void opMoveReg(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;

    rd = rdOf(inst);
    rs = rsOf(inst);

    m->regs[rd] = m->regs[rs];
    m->pc = m->pc + 4;
}

static void opMoveImm(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t imm;
    uint64_t before;
    uint64_t after;

    rd = rdOf(inst);
    imm = imm12Of(inst);

    before = m->regs[rd];
    after = before & ~0xFFFULL;
    after = after | ((uint64_t)imm & 0xFFFULL);

    m->regs[rd] = after;
    m->pc = m->pc + 4;
}

static void opStore(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    int64_t offset;
    int64_t addrSigned;
    uint64_t addr;

    rd = rdOf(inst);
    rs = rsOf(inst);

    offset = signExtend12(imm12Of(inst));
    addrSigned = (int64_t)m->regs[rd] + offset;
    addr = checkedAddress(addrSigned, 8);

    writeU64LE(m, addr, m->regs[rs]);
    m->pc = m->pc + 4;
}

static void opAddF(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = bitsToDouble(m->regs[rs]);
    b = bitsToDouble(m->regs[rt]);
    c = a + b;

    m->regs[rd] = doubleToBits(c);
    m->pc = m->pc + 4;
}

static void opSubF(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = bitsToDouble(m->regs[rs]);
    b = bitsToDouble(m->regs[rt]);
    c = a - b;

    m->regs[rd] = doubleToBits(c);
    m->pc = m->pc + 4;
}

static void opMulF(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = bitsToDouble(m->regs[rs]);
    b = bitsToDouble(m->regs[rt]);
    c = a * b;

    m->regs[rd] = doubleToBits(c);
    m->pc = m->pc + 4;
}

static void opDivF(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = bitsToDouble(m->regs[rs]);
    b = bitsToDouble(m->regs[rt]);

    if (b == 0.0)
    {
        stopSimError();
    }

    c = a / b;

    m->regs[rd] = doubleToBits(c);
    m->pc = m->pc + 4;
}

static void opAddI(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = (int64_t)m->regs[rs];
    b = (int64_t)m->regs[rt];
    c = a + b;

    m->regs[rd] = (uint64_t)c;
    m->pc = m->pc + 4;
}

static void opAddImm(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t imm;

    rd = rdOf(inst);
    imm = imm12Of(inst);

    m->regs[rd] = m->regs[rd] + (uint64_t)imm;
    m->pc = m->pc + 4;
}

static void opSubI(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = (int64_t)m->regs[rs];
    b = (int64_t)m->regs[rt];
    c = a - b;

    m->regs[rd] = (uint64_t)c;
    m->pc = m->pc + 4;
}

static void opSubImm(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t imm;

    rd = rdOf(inst);
    imm = imm12Of(inst);

    m->regs[rd] = m->regs[rd] - (uint64_t)imm;
    m->pc = m->pc + 4;
}

static void opMulI(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = (int64_t)m->regs[rs];
    b = (int64_t)m->regs[rt];
    c = a * b;

    m->regs[rd] = (uint64_t)c;
    m->pc = m->pc + 4;
}

static void opDivI(Machine *m, uint32_t inst)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t c;

    rd = rdOf(inst);
    rs = rsOf(inst);
    rt = rtOf(inst);

    a = (int64_t)m->regs[rs];
    b = (int64_t)m->regs[rt];

    if (b == 0)
    {
        stopSimError();
    }

    c = a / b;

    m->regs[rd] = (uint64_t)c;
    m->pc = m->pc + 4;
}

static void buildOps(OpFn ops[32])
{
    int i;

    i = 0;
    while (i < 32)
    {
        ops[i] = opIllegal;
        i++;
    }

    ops[0x00] = opAnd;
    ops[0x01] = opOr;
    ops[0x02] = opXor;
    ops[0x03] = opNot;

    ops[0x04] = opShiftRightReg;
    ops[0x05] = opShiftRightImm;
    ops[0x06] = opShiftLeftReg;
    ops[0x07] = opShiftLeftImm;

    ops[0x08] = opBranchAbs;
    ops[0x09] = opBranchRelReg;
    ops[0x0A] = opBranchRelImm;
    ops[0x0B] = opBranchNotZero;
    ops[0x0C] = opCall;
    ops[0x0D] = opReturn;
    ops[0x0E] = opBranchGreaterThan;

    ops[0x0F] = opPriv;

    ops[0x10] = opLoad;
    ops[0x11] = opMoveReg;
    ops[0x12] = opMoveImm;
    ops[0x13] = opStore;

    ops[0x14] = opAddF;
    ops[0x15] = opSubF;
    ops[0x16] = opMulF;
    ops[0x17] = opDivF;

    ops[0x18] = opAddI;
    ops[0x19] = opAddImm;
    ops[0x1A] = opSubI;
    ops[0x1B] = opSubImm;
    ops[0x1C] = opMulI;
    ops[0x1D] = opDivI;
}

static void run(Machine *m)
{
    OpFn ops[32];

    buildOps(ops);

    while (m->stopped == false)
    {
        uint64_t safePc;
        uint32_t inst;
        uint32_t op;

        safePc = checkedAddress((int64_t)m->pc, 4);
        inst = readU32LE(m, safePc);

        op = opcodeOf(inst);
        ops[op](m, inst);
    }
}

int main(int argc, char **argv)
{
    Machine m;
    uint8_t *memory;

    if (argc != 2)
    {
        stopBadPath();
    }

    memory = (uint8_t *)calloc((size_t)memoryBytes, 1);
    if (memory == NULL)
    {
        stopSimError();
    }

    memset(&m, 0, sizeof(m));
    m.memory = memory;
    m.regs[31] = memoryBytes;

    loadTko(&m, argv[1]);
    run(&m);

    free(memory);
    return 0;
}