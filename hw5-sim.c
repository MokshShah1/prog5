#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

static const uint64_t ramSizeBytes = 512ULL * 1024ULL;
static const uint64_t requiredCodeBase = 0x2000ULL;
static const uint64_t requiredDataBase = 0x10000ULL;

typedef struct
{
    uint8_t *ram;
    uint64_t regs[32];
    uint64_t pc;
    bool halted;
} CpuState;

typedef void (*InstructionFn)(CpuState *, uint32_t);

static void failBadFilepath(void)
{
    fprintf(stderr, "Invalid tinker filepath\n");
    exit(1);
}

static void failSimulation(void)
{
    fprintf(stderr, "Simulation error\n");
    exit(1);
}

static uint64_t requireValidAddress(int64_t signedAddress, uint64_t bytesNeeded)
{
    uint64_t address;

    if (signedAddress < 0)
    {
        failSimulation();
    }

    address = (uint64_t)signedAddress;

    if (address + bytesNeeded > ramSizeBytes)
    {
        failSimulation();
    }

    return address;
}

static uint32_t readU32LittleEndian(CpuState *cpu, uint64_t address)
{
    uint32_t b0;
    uint32_t b1;
    uint32_t b2;
    uint32_t b3;
    uint32_t value;

    b0 = (uint32_t)cpu->ram[address + 0];
    b1 = (uint32_t)cpu->ram[address + 1];
    b2 = (uint32_t)cpu->ram[address + 2];
    b3 = (uint32_t)cpu->ram[address + 3];

    value = 0;
    value |= b0;
    value |= (b1 << 8);
    value |= (b2 << 16);
    value |= (b3 << 24);

    return value;
}

static uint64_t readU64LittleEndian(CpuState *cpu, uint64_t address)
{
    uint64_t value;
    int i;

    value = 0;
    i = 0;

    while (i < 8)
    {
        uint64_t oneByte;
        uint64_t shift;

        oneByte = (uint64_t)cpu->ram[address + (uint64_t)i];
        shift = (uint64_t)(8 * i);

        value |= (oneByte << shift);
        i++;
    }

    return value;
}

static void writeU64LittleEndian(CpuState *cpu, uint64_t address, uint64_t value)
{
    int i;

    i = 0;
    while (i < 8)
    {
        uint64_t shift;
        uint64_t oneByte;

        shift = (uint64_t)(8 * i);
        oneByte = (value >> shift) & 0xFFULL;

        cpu->ram[address + (uint64_t)i] = (uint8_t)oneByte;
        i++;
    }
}

static int64_t signExtendImm12(uint32_t imm12)
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

static uint32_t getOpcode(uint32_t instruction)
{
    uint32_t value;

    value = (instruction >> 27) & 0x1Fu;
    return value;
}

static uint32_t getRd(uint32_t instruction)
{
    uint32_t value;

    value = (instruction >> 22) & 0x1Fu;
    return value;
}

static uint32_t getRs(uint32_t instruction)
{
    uint32_t value;

    value = (instruction >> 17) & 0x1Fu;
    return value;
}

static uint32_t getRt(uint32_t instruction)
{
    uint32_t value;

    value = (instruction >> 12) & 0x1Fu;
    return value;
}

static uint32_t getImm12(uint32_t instruction)
{
    uint32_t value;

    value = instruction & 0xFFFu;
    return value;
}

static double bitsToFloat64(uint64_t bits)
{
    double value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t float64ToBits(double value)
{
    uint64_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t readUnsignedFromStdinStrict(void)
{
    char text[256];
    char *end;
    unsigned long long parsed;
    uint64_t result;

    if (scanf("%255s", text) != 1)
    {
        failSimulation();
    }

    if (text[0] == '-' || text[0] == '+')
    {
        failSimulation();
    }

    errno = 0;
    end = NULL;
    parsed = strtoull(text, &end, 10);

    if (errno != 0)
    {
        failSimulation();
    }

    if (end == NULL)
    {
        failSimulation();
    }

    if (*end != '\0')
    {
        failSimulation();
    }

    result = (uint64_t)parsed;
    return result;
}

static uint64_t readU64LittleEndianFromFile(FILE *file)
{
    uint8_t bytes[8];
    size_t got;
    uint64_t value;
    int i;

    got = fread(bytes, 1, 8, file);
    if (got != 8)
    {
        failBadFilepath();
    }

    value = 0;
    i = 0;
    while (i < 8)
    {
        value |= ((uint64_t)bytes[i]) << (uint64_t)(8 * i);
        i++;
    }

    return value;
}

static void readExactBytes(FILE *file, uint8_t *dst, uint64_t count)
{
    size_t got;

    if (count == 0)
    {
        return;
    }

    got = fread(dst, 1, (size_t)count, file);
    if (got != (size_t)count)
    {
        failBadFilepath();
    }
}

static void loadProgramImage(CpuState *cpu, const char *path)
{
    FILE *file;
    uint64_t fileType;
    uint64_t codeBase;
    uint64_t codeBytes;
    uint64_t dataBase;
    uint64_t dataBytes;

    uint64_t codeEnd;
    uint64_t dataEnd;

    file = fopen(path, "rb");
    if (file == NULL)
    {
        failBadFilepath();
    }

    fileType = readU64LittleEndianFromFile(file);
    codeBase = readU64LittleEndianFromFile(file);
    codeBytes = readU64LittleEndianFromFile(file);
    dataBase = readU64LittleEndianFromFile(file);
    dataBytes = readU64LittleEndianFromFile(file);

    if (fileType != 0ULL)
    {
        fclose(file);
        failSimulation();
    }

    if (codeBase != requiredCodeBase)
    {
        fclose(file);
        failSimulation();
    }

    if (dataBase != requiredDataBase)
    {
        fclose(file);
        failSimulation();
    }

    if ((codeBytes % 4ULL) != 0ULL)
    {
        fclose(file);
        failSimulation();
    }

    if ((dataBytes % 8ULL) != 0ULL)
    {
        fclose(file);
        failSimulation();
    }

    codeEnd = codeBase + codeBytes;
    dataEnd = dataBase + dataBytes;

    if (codeEnd > ramSizeBytes)
    {
        fclose(file);
        failSimulation();
    }

    if (dataEnd > ramSizeBytes)
    {
        fclose(file);
        failSimulation();
    }

    if (codeBytes != 0ULL && dataBytes != 0ULL)
    {
        if (codeBase < dataEnd && dataBase < codeEnd)
        {
            fclose(file);
            failSimulation();
        }
    }

    readExactBytes(file, cpu->ram + codeBase, codeBytes);
    readExactBytes(file, cpu->ram + dataBase, dataBytes);

    fclose(file);

    cpu->pc = codeBase;
}

static void executeIllegal(CpuState *cpu, uint32_t instruction)
{
    (void)cpu;
    (void)instruction;
    failSimulation();
}

static void executeAnd(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    cpu->regs[rd] = cpu->regs[rs] & cpu->regs[rt];
    cpu->pc = cpu->pc + 4;
}

static void executeOr(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    cpu->regs[rd] = cpu->regs[rs] | cpu->regs[rt];
    cpu->pc = cpu->pc + 4;
}

static void executeXor(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    cpu->regs[rd] = cpu->regs[rs] ^ cpu->regs[rt];
    cpu->pc = cpu->pc + 4;
}

static void executeNot(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;

    rd = getRd(instruction);
    rs = getRs(instruction);

    cpu->regs[rd] = ~cpu->regs[rs];
    cpu->pc = cpu->pc + 4;
}

static void executeShiftRightRegister(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    uint64_t shiftAmount;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    shiftAmount = cpu->regs[rt] & 63ULL;
    cpu->regs[rd] = cpu->regs[rs] >> shiftAmount;
    cpu->pc = cpu->pc + 4;
}

static void executeShiftRightImmediate(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint64_t shiftAmount;

    rd = getRd(instruction);

    shiftAmount = (uint64_t)(getImm12(instruction) & 63u);
    cpu->regs[rd] = cpu->regs[rd] >> shiftAmount;
    cpu->pc = cpu->pc + 4;
}

static void executeShiftLeftRegister(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    uint64_t shiftAmount;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    shiftAmount = cpu->regs[rt] & 63ULL;
    cpu->regs[rd] = cpu->regs[rs] << shiftAmount;
    cpu->pc = cpu->pc + 4;
}

static void executeShiftLeftImmediate(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint64_t shiftAmount;

    rd = getRd(instruction);

    shiftAmount = (uint64_t)(getImm12(instruction) & 63u);
    cpu->regs[rd] = cpu->regs[rd] << shiftAmount;
    cpu->pc = cpu->pc + 4;
}

static void executeBranchAbsolute(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;

    rd = getRd(instruction);
    cpu->pc = cpu->regs[rd];
}

static void executeBranchRelativeRegister(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint64_t offset;

    rd = getRd(instruction);
    offset = cpu->regs[rd];

    cpu->pc = cpu->pc + offset;
}

static void executeBranchRelativeImmediate(CpuState *cpu, uint32_t instruction)
{
    int64_t offset;
    uint64_t nextPc;

    offset = signExtendImm12(getImm12(instruction));
    nextPc = (uint64_t)((int64_t)cpu->pc + offset);

    cpu->pc = nextPc;
}

static void executeBranchNotZero(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;

    rd = getRd(instruction);
    rs = getRs(instruction);

    if (cpu->regs[rs] == 0ULL)
    {
        cpu->pc = cpu->pc + 4;
    }
    else
    {
        cpu->pc = cpu->regs[rd];
    }
}

static void executeCall(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint64_t stackPointer;
    int64_t returnAddrSlotSigned;
    uint64_t returnAddrSlot;

    rd = getRd(instruction);
    stackPointer = cpu->regs[31];

    returnAddrSlotSigned = (int64_t)stackPointer - 8;
    returnAddrSlot = requireValidAddress(returnAddrSlotSigned, 8);

    writeU64LittleEndian(cpu, returnAddrSlot, cpu->pc + 4);

    cpu->pc = cpu->regs[rd];
}

static void executeReturn(CpuState *cpu, uint32_t instruction)
{
    uint64_t stackPointer;
    int64_t returnAddrSlotSigned;
    uint64_t returnAddrSlot;
    uint64_t returnPc;

    (void)instruction;

    stackPointer = cpu->regs[31];

    returnAddrSlotSigned = (int64_t)stackPointer - 8;
    returnAddrSlot = requireValidAddress(returnAddrSlotSigned, 8);

    returnPc = readU64LittleEndian(cpu, returnAddrSlot);

    cpu->pc = returnPc;
}

static void executeBranchGreaterThan(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t left;
    int64_t right;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    left = (int64_t)cpu->regs[rs];
    right = (int64_t)cpu->regs[rt];

    if (left > right)
    {
        cpu->pc = cpu->regs[rd];
    }
    else
    {
        cpu->pc = cpu->pc + 4;
    }
}

static void executePrivileged(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t imm;
    uint64_t portValue;

    rd = getRd(instruction);
    rs = getRs(instruction);
    imm = getImm12(instruction);

    if (imm == 0u)
    {
        cpu->halted = true;
        return;
    }

    if (imm == 3u)
    {
        portValue = cpu->regs[rs];

        if (portValue == 0ULL)
        {
            cpu->regs[rd] = readUnsignedFromStdinStrict();
        }

        cpu->pc = cpu->pc + 4;
        return;
    }

    if (imm == 4u)
    {
        portValue = cpu->regs[rd];

        if (portValue == 1ULL)
        {
            printf("%llu\n", (unsigned long long)cpu->regs[rs]);
        }
        else if (portValue == 3ULL)
        {
            putchar((int)(cpu->regs[rs] & 0xFFULL));
            fflush(stdout);
        }

        cpu->pc = cpu->pc + 4;
        return;
    }

    failSimulation();
}

static void executeLoad(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    int64_t offset;
    int64_t addrSigned;
    uint64_t addr;

    rd = getRd(instruction);
    rs = getRs(instruction);

    offset = signExtendImm12(getImm12(instruction));
    addrSigned = (int64_t)cpu->regs[rs] + offset;
    addr = requireValidAddress(addrSigned, 8);

    cpu->regs[rd] = readU64LittleEndian(cpu, addr);
    cpu->pc = cpu->pc + 4;
}

static void executeMoveRegister(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;

    rd = getRd(instruction);
    rs = getRs(instruction);

    cpu->regs[rd] = cpu->regs[rs];
    cpu->pc = cpu->pc + 4;
}

static void executeMoveImmediate(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t imm;
    uint64_t current;
    uint64_t updated;

    rd = getRd(instruction);
    imm = getImm12(instruction);

    current = cpu->regs[rd];
    updated = current & ~0xFFFULL;
    updated = updated | ((uint64_t)imm & 0xFFFULL);

    cpu->regs[rd] = updated;
    cpu->pc = cpu->pc + 4;
}

static void executeStore(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    int64_t offset;
    int64_t addrSigned;
    uint64_t addr;

    rd = getRd(instruction);
    rs = getRs(instruction);

    offset = signExtendImm12(getImm12(instruction));
    addrSigned = (int64_t)cpu->regs[rd] + offset;
    addr = requireValidAddress(addrSigned, 8);

    writeU64LittleEndian(cpu, addr, cpu->regs[rs]);
    cpu->pc = cpu->pc + 4;
}

static void executeAddFloat(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = bitsToFloat64(cpu->regs[rs]);
    b = bitsToFloat64(cpu->regs[rt]);
    result = a + b;

    cpu->regs[rd] = float64ToBits(result);
    cpu->pc = cpu->pc + 4;
}

static void executeSubFloat(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = bitsToFloat64(cpu->regs[rs]);
    b = bitsToFloat64(cpu->regs[rt]);
    result = a - b;

    cpu->regs[rd] = float64ToBits(result);
    cpu->pc = cpu->pc + 4;
}

static void executeMulFloat(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = bitsToFloat64(cpu->regs[rs]);
    b = bitsToFloat64(cpu->regs[rt]);
    result = a * b;

    cpu->regs[rd] = float64ToBits(result);
    cpu->pc = cpu->pc + 4;
}

static void executeDivFloat(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    double a;
    double b;
    double result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = bitsToFloat64(cpu->regs[rs]);
    b = bitsToFloat64(cpu->regs[rt]);

    if (b == 0.0)
    {
        failSimulation();
    }

    result = a / b;

    cpu->regs[rd] = float64ToBits(result);
    cpu->pc = cpu->pc + 4;
}

static void executeAddInt(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = (int64_t)cpu->regs[rs];
    b = (int64_t)cpu->regs[rt];
    result = a + b;

    cpu->regs[rd] = (uint64_t)result;
    cpu->pc = cpu->pc + 4;
}

static void executeAddImmediate(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t imm;

    rd = getRd(instruction);
    imm = getImm12(instruction);

    cpu->regs[rd] = cpu->regs[rd] + (uint64_t)imm;
    cpu->pc = cpu->pc + 4;
}

static void executeSubInt(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = (int64_t)cpu->regs[rs];
    b = (int64_t)cpu->regs[rt];
    result = a - b;

    cpu->regs[rd] = (uint64_t)result;
    cpu->pc = cpu->pc + 4;
}

static void executeSubImmediate(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t imm;

    rd = getRd(instruction);
    imm = getImm12(instruction);

    cpu->regs[rd] = cpu->regs[rd] - (uint64_t)imm;
    cpu->pc = cpu->pc + 4;
}

static void executeMulInt(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = (int64_t)cpu->regs[rs];
    b = (int64_t)cpu->regs[rt];
    result = a * b;

    cpu->regs[rd] = (uint64_t)result;
    cpu->pc = cpu->pc + 4;
}

static void executeDivInt(CpuState *cpu, uint32_t instruction)
{
    uint32_t rd;
    uint32_t rs;
    uint32_t rt;
    int64_t a;
    int64_t b;
    int64_t result;

    rd = getRd(instruction);
    rs = getRs(instruction);
    rt = getRt(instruction);

    a = (int64_t)cpu->regs[rs];
    b = (int64_t)cpu->regs[rt];

    if (b == 0)
    {
        failSimulation();
    }

    result = a / b;

    cpu->regs[rd] = (uint64_t)result;
    cpu->pc = cpu->pc + 4;
}

static void buildInstructionTable(InstructionFn table[32])
{
    int i;

    i = 0;
    while (i < 32)
    {
        table[i] = executeIllegal;
        i++;
    }

    table[0x00] = executeAnd;
    table[0x01] = executeOr;
    table[0x02] = executeXor;
    table[0x03] = executeNot;

    table[0x04] = executeShiftRightRegister;
    table[0x05] = executeShiftRightImmediate;
    table[0x06] = executeShiftLeftRegister;
    table[0x07] = executeShiftLeftImmediate;

    table[0x08] = executeBranchAbsolute;
    table[0x09] = executeBranchRelativeRegister;
    table[0x0A] = executeBranchRelativeImmediate;
    table[0x0B] = executeBranchNotZero;
    table[0x0C] = executeCall;
    table[0x0D] = executeReturn;
    table[0x0E] = executeBranchGreaterThan;

    table[0x0F] = executePrivileged;

    table[0x10] = executeLoad;
    table[0x11] = executeMoveRegister;
    table[0x12] = executeMoveImmediate;
    table[0x13] = executeStore;

    table[0x14] = executeAddFloat;
    table[0x15] = executeSubFloat;
    table[0x16] = executeMulFloat;
    table[0x17] = executeDivFloat;

    table[0x18] = executeAddInt;
    table[0x19] = executeAddImmediate;
    table[0x1A] = executeSubInt;
    table[0x1B] = executeSubImmediate;
    table[0x1C] = executeMulInt;
    table[0x1D] = executeDivInt;
}

static void runMachine(CpuState *cpu)
{
    InstructionFn instructions[32];

    buildInstructionTable(instructions);

    while (cpu->halted == false)
    {
        uint64_t safePc;
        uint32_t instruction;
        uint32_t opcode;

        safePc = requireValidAddress((int64_t)cpu->pc, 4);
        instruction = readU32LittleEndian(cpu, safePc);

        opcode = getOpcode(instruction);
        instructions[opcode](cpu, instruction);
    }
}

int main(int argc, char **argv)
{
    CpuState cpu;
    uint8_t *ram;

    if (argc != 2)
    {
        failBadFilepath();
    }

    ram = (uint8_t *)calloc((size_t)ramSizeBytes, 1);
    if (ram == NULL)
    {
        failSimulation();
    }

    memset(&cpu, 0, sizeof(cpu));
    cpu.ram = ram;
    cpu.regs[31] = ramSizeBytes;

    loadProgramImage(&cpu, argv[1]);
    runMachine(&cpu);

    free(ram);
    return 0;
}