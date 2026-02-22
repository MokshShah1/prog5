#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

typedef struct
{
    int total;
    int failed;
    bool currentFailed;
} TestStats;

static TestStats g_stats;

static void failHarness(const char *message)
{
    fprintf(stderr, "TEST HARNESS ERROR: %s\n", message);
    exit(2);
}

static bool reportFailureAt(const char *file, int line, const char *message)
{
    fprintf(stderr, "FAIL at %s:%d: %s\n", file, line, message);
    g_stats.currentFailed = true;
    return false;
}

static bool expectTrueAt(const char *file, int line, bool value, const char *exprText)
{
    if (value)
    {
        return true;
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg), "expected true: %s", exprText);
        return reportFailureAt(file, line, msg);
    }
}

static bool expectFalseAt(const char *file, int line, bool value, const char *exprText)
{
    return expectTrueAt(file, line, !value, exprText);
}

static bool expectEqU64At(const char *file, int line, uint64_t a, uint64_t b, const char *aText, const char *bText)
{
    if (a == b)
    {
        return true;
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "expected equal u64: %s=%llu, %s=%llu",
                 aText, (unsigned long long)a, bText, (unsigned long long)b);
        return reportFailureAt(file, line, msg);
    }
}

static bool expectEqI64At(const char *file, int line, long long a, long long b, const char *aText, const char *bText)
{
    if (a == b)
    {
        return true;
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "expected equal i64: %s=%lld, %s=%lld",
                 aText, a, bText, b);
        return reportFailureAt(file, line, msg);
    }
}

static bool expectEqIntAt(const char *file, int line, int a, int b, const char *aText, const char *bText)
{
    if (a == b)
    {
        return true;
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "expected equal int: %s=%d, %s=%d",
                 aText, a, bText, b);
        return reportFailureAt(file, line, msg);
    }
}

static bool expectStrEqAt(const char *file, int line, const char *a, const char *b)
{
    bool same;

    if (a == NULL && b == NULL)
    {
        return true;
    }

    if (a == NULL || b == NULL)
    {
        same = false;
    }
    else
    {
        same = (strcmp(a, b) == 0);
    }

    if (same)
    {
        return true;
    }

    {
        const char *sa = (a != NULL) ? a : "(null)";
        const char *sb = (b != NULL) ? b : "(null)";
        char msg[1024];
        snprintf(msg, sizeof(msg), "expected strings equal:\n  got: \"%s\"\n  exp: \"%s\"", sa, sb);
        return reportFailureAt(file, line, msg);
    }
}

static void writeTextFile(const char *path, const char *text)
{
    FILE *file;

    file = fopen(path, "wb");
    if (file == NULL)
    {
        failHarness("cannot create temp file");
    }

    if (text != NULL && text[0] != '\0')
    {
        size_t len;
        size_t wrote;

        len = strlen(text);
        wrote = fwrite(text, 1, len, file);
        if (wrote != len)
        {
            fclose(file);
            failHarness("failed writing temp file");
        }
    }

    fclose(file);
}

static char *readAllFile(const char *path)
{
    FILE *file;
    long n;
    char *buf;
    size_t got;

    file = fopen(path, "rb");
    if (file == NULL)
    {
        failHarness("cannot open file to read");
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        failHarness("fseek end");
    }

    n = ftell(file);
    if (n < 0)
    {
        fclose(file);
        failHarness("ftell");
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        failHarness("fseek set");
    }

    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL)
    {
        fclose(file);
        failHarness("out of memory");
    }

    got = fread(buf, 1, (size_t)n, file);
    fclose(file);

    if (got != (size_t)n)
    {
        free(buf);
        failHarness("short read");
    }

    buf[n] = '\0';
    return buf;
}

typedef struct
{
    int savedStdin;
    int savedStdout;
} StdioBackup;

static int dupFd(int fd)
{
#if defined(_WIN32)
    return _dup(fd);
#else
    return dup(fd);
#endif
}

static int dup2Fd(int src, int dst)
{
#if defined(_WIN32)
    return _dup2(src, dst);
#else
    return dup2(src, dst);
#endif
}

static int closeFd(int fd)
{
#if defined(_WIN32)
    return _close(fd);
#else
    return close(fd);
#endif
}

static int filenoOf(FILE *f)
{
#if defined(_WIN32)
    return _fileno(f);
#else
    return fileno(f);
#endif
}

static void beginRedirect(StdioBackup *backup, const char *stdinPath, const char *stdoutPath)
{
    backup->savedStdin = dupFd(filenoOf(stdin));
    backup->savedStdout = dupFd(filenoOf(stdout));

    if (backup->savedStdin < 0 || backup->savedStdout < 0)
    {
        failHarness("dup failed");
    }

    if (stdinPath != NULL)
    {
        FILE *nf;
        nf = freopen(stdinPath, "rb", stdin);
        if (nf == NULL)
        {
            failHarness("freopen stdin failed");
        }
    }

    if (stdoutPath != NULL)
    {
        FILE *nf;
        nf = freopen(stdoutPath, "wb", stdout);
        if (nf == NULL)
        {
            failHarness("freopen stdout failed");
        }
    }
}

static void endRedirect(StdioBackup *backup)
{
    fflush(stdout);

    if (dup2Fd(backup->savedStdin, filenoOf(stdin)) != 0)
    {
        failHarness("restore stdin failed");
    }

    if (dup2Fd(backup->savedStdout, filenoOf(stdout)) != 0)
    {
        failHarness("restore stdout failed");
    }

    closeFd(backup->savedStdin);
    closeFd(backup->savedStdout);

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
}

static const char *assemblerExe(void)
{
#if defined(_WIN32)
    return "hw5-asm.exe";
#else
    return "./hw5-asm";
#endif
}

static const char *simulatorExe(void)
{
#if defined(_WIN32)
    return "hw5-sim.exe";
#else
    return "./hw5-sim";
#endif
}

static int runCommand(const char *commandLine)
{
    int rc;

    rc = system(commandLine);
    return rc;
}

static int assembleFile(const char *tkPath, const char *tkoPath, const char *tkText)
{
    char cmd[1024];

    writeTextFile(tkPath, tkText);

    snprintf(cmd, sizeof(cmd), "%s %s %s", assemblerExe(), tkPath, tkoPath);
    return runCommand(cmd);
}

static char *runSimulatorCapture(const char *tkoPath, const char *stdinPath, const char *stdoutPath, const char *stdinText)
{
    char cmd[1024];
    StdioBackup backup;
    int rc;

    writeTextFile(stdinPath, stdinText);

    beginRedirect(&backup, stdinPath, stdoutPath);

    snprintf(cmd, sizeof(cmd), "%s %s", simulatorExe(), tkoPath);
    rc = runCommand(cmd);
    (void)rc;

    endRedirect(&backup);

    return readAllFile(stdoutPath);
}

static uint64_t doubleBits(double value)
{
    uint64_t bits;

    bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

typedef bool (*TestFn)(void);

typedef struct
{
    const char *name;
    TestFn fn;
} TestCase;

static bool testIntegrationBinarySearch(void)
{
    const char *tk =
        ".code\n"
        "\tld r1, 0\n"
        "\tld r2, 3\n"
        "\tclr r8\n"
        "\tin r3, r1\n"
        "\tld r15, 65536\n"
        "\tclr r6\n"
        ":read_check\n"
        "\tld r20, :read_body\n"
        "\tbrgt r20, r3, r6\n"
        "\tld r20, :after_read\n"
        "\tbr r20\n"
        ":read_body\n"
        "\tin r7, r1\n"
        "\tmov r11, r6\n"
        "\tshftli r11, 3\n"
        "\tadd r11, r15, r11\n"
        "\tmov (r11)(0), r7\n"
        "\taddi r6, 1\n"
        "\tld r20, :read_check\n"
        "\tbr r20\n"
        ":after_read\n"
        "\tin r4, r1\n"
        "\tld r20, :bs_start\n"
        "\tbrgt r20, r3, r8\n"
        "\tld r20, :print_not_found\n"
        "\tbr r20\n"
        ":bs_start\n"
        "\tclr r5\n"
        "\tmov r6, r3\n"
        "\tsubi r6, 1\n"
        ":bs_check\n"
        "\tld r20, :print_not_found\n"
        "\tbrgt r20, r5, r6\n"
        "\tmov r7, r5\n"
        "\tadd r7, r7, r6\n"
        "\tshftri r7, 1\n"
        "\tmov r11, r7\n"
        "\tshftli r11, 3\n"
        "\tadd r11, r15, r11\n"
        "\tmov r9, (r11)(0)\n"
        "\tld r20, :go_right\n"
        "\tbrgt r20, r4, r9\n"
        "\tld r20, :go_left\n"
        "\tbrgt r20, r9, r4\n"
        "\tld r20, :print_found\n"
        "\tbr r20\n"
        ":go_right\n"
        "\tmov r5, r7\n"
        "\taddi r5, 1\n"
        "\tld r20, :bs_check\n"
        "\tbr r20\n"
        ":go_left\n"
        "\tmov r6, r7\n"
        "\tsubi r6, 1\n"
        "\tld r20, :bs_check\n"
        "\tbr r20\n"
        ":print_found\n"
        "\tld r10, 102\n"
        "\tout r2, r10\n"
        "\tld r10, 111\n"
        "\tout r2, r10\n"
        "\tld r10, 117\n"
        "\tout r2, r10\n"
        "\tld r10, 110\n"
        "\tout r2, r10\n"
        "\tld r10, 100\n"
        "\tout r2, r10\n"
        "\thalt\n"
        ":print_not_found\n"
        "\tld r10, 110\n"
        "\tout r2, r10\n"
        "\tld r10, 111\n"
        "\tout r2, r10\n"
        "\tld r10, 116\n"
        "\tout r2, r10\n"
        "\tld r10, 32\n"
        "\tout r2, r10\n"
        "\tld r10, 102\n"
        "\tout r2, r10\n"
        "\tld r10, 111\n"
        "\tout r2, r10\n"
        "\tld r10, 117\n"
        "\tout r2, r10\n"
        "\tld r10, 110\n"
        "\tout r2, r10\n"
        "\tld r10, 100\n"
        "\tout r2, r10\n"
        "\thalt\n";

    const char *tkPath = "tmp_binsearch.tk";
    const char *tkoPath = "tmp_binsearch.tko";
    const char *inPath = "tmp_in.txt";
    const char *outPath = "tmp_out.txt";

    int rc;
    char *out1;
    char *out2;

    rc = assembleFile(tkPath, tkoPath, tk);
    if (!expectEqIntAt(__FILE__, __LINE__, rc, 0, "assembler rc", "0"))
    {
        return false;
    }

    out1 = runSimulatorCapture(tkoPath, inPath, outPath, "3 1 5 9 5\n");
    if (!expectStrEqAt(__FILE__, __LINE__, out1, "found"))
    {
        free(out1);
        return false;
    }
    free(out1);

    out2 = runSimulatorCapture(tkoPath, inPath, outPath, "3 1 5 9 2\n");
    if (!expectStrEqAt(__FILE__, __LINE__, out2, "not found"))
    {
        free(out2);
        return false;
    }
    free(out2);

    return true;
}

static bool testIntegrationFibonacci(void)
{
    const char *tk =
        ".code\n"
        "\tld r1, 0\n"
        "\tld r2, 1\n"
        "\tin r3, r1\n"
        "\n"
        "\tld r7, 2\n"
        "\tld r20, :compute\n"
        "\tbrgt r20, r3, r7\n"
        "\n"
        "\tld r7, 1\n"
        "\tld r20, :print_one\n"
        "\tbrgt r20, r3, r7\n"
        "\n"
        "\tclr r4\n"
        "\tout r2, r4\n"
        "\tpriv r0, r0, r0, 0\n"
        "\n"
        ":print_one\n"
        "\tld r4, 1\n"
        "\tout r2, r4\n"
        "\tpriv r0, r0, r0, 0\n"
        "\n"
        ":compute\n"
        "\tclr r4\n"
        "\tld r5, 1\n"
        "\n"
        "\tmov r6, r3\n"
        "\tsubi r6, 2\n"
        "\n"
        "\tclr r8\n"
        "\n"
        ":loop_check\n"
        "\tld r20, :loop_body\n"
        "\tbrgt r20, r6, r8\n"
        "\n"
        "\tout r2, r5\n"
        "\tpriv r0, r0, r0, 0\n"
        "\n"
        ":loop_body\n"
        "\tadd r7, r4, r5\n"
        "\tmov r4, r5\n"
        "\tmov r5, r7\n"
        "\tsubi r6, 1\n"
        "\tld r20, :loop_check\n"
        "\tbr r20\n";

    const char *tkPath = "tmp_fib.tk";
    const char *tkoPath = "tmp_fib.tko";
    const char *inPath = "tmp_in.txt";
    const char *outPath = "tmp_out.txt";

    int rc;
    char *o0;
    char *o1;
    char *o4;
    char *o6;

    rc = assembleFile(tkPath, tkoPath, tk);
    if (!expectEqIntAt(__FILE__, __LINE__, rc, 0, "assembler rc", "0"))
    {
        return false;
    }

    o0 = runSimulatorCapture(tkoPath, inPath, outPath, "0\n");
    if (!expectStrEqAt(__FILE__, __LINE__, o0, "0\n"))
    {
        free(o0);
        return false;
    }
    free(o0);

    o1 = runSimulatorCapture(tkoPath, inPath, outPath, "1\n");
    if (!expectStrEqAt(__FILE__, __LINE__, o1, "1\n"))
    {
        free(o1);
        return false;
    }
    free(o1);

    o4 = runSimulatorCapture(tkoPath, inPath, outPath, "4\n");
    if (!expectStrEqAt(__FILE__, __LINE__, o4, "2\n"))
    {
        free(o4);
        return false;
    }
    free(o4);

    o6 = runSimulatorCapture(tkoPath, inPath, outPath, "6\n");
    if (!expectStrEqAt(__FILE__, __LINE__, o6, "5\n"))
    {
        free(o6);
        return false;
    }
    free(o6);

    return true;
}

static bool testIntegrationMatrixMulN1(void)
{
    const char *tk =
        ".code\n"
        "\tld r1, 0\n"
        "\tld r2, 1\n"
        "\tclr r8\n"
        "\n"
        "\tin r3, r1\n"
        "\n"
        "\tld r20, :start\n"
        "\tbrgt r20, r3, r8\n"
        "\tpriv r0, r0, r0, 0\n"
        "\n"
        ":start\n"
        "\tmul r4, r3, r3\n"
        "\n"
        "\tld r15, 65536\n"
        "\n"
        "\tclr r6\n"
        "\n"
        ":readA_check\n"
        "\tld r20, :readA_body\n"
        "\tbrgt r20, r4, r6\n"
        "\tld r20, :readB_setup\n"
        "\tbr r20\n"
        "\n"
        ":readA_body\n"
        "\tin r7, r1\n"
        "\tmov r11, r6\n"
        "\tshftli r11, 3\n"
        "\tadd r11, r15, r11\n"
        "\tmov (r11)(0), r7\n"
        "\taddi r6, 1\n"
        "\tld r20, :readA_check\n"
        "\tbr r20\n"
        "\n"
        ":readB_setup\n"
        "\tmov r16, r4\n"
        "\tshftli r16, 3\n"
        "\tadd r16, r15, r16\n"
        "\n"
        "\tclr r6\n"
        "\n"
        ":readB_check\n"
        "\tld r20, :readB_body\n"
        "\tbrgt r20, r4, r6\n"
        "\tld r20, :mul_setup\n"
        "\tbr r20\n"
        "\n"
        ":readB_body\n"
        "\tin r7, r1\n"
        "\tmov r11, r6\n"
        "\tshftli r11, 3\n"
        "\tadd r11, r16, r11\n"
        "\tmov (r11)(0), r7\n"
        "\taddi r6, 1\n"
        "\tld r20, :readB_check\n"
        "\tbr r20\n"
        "\n"
        ":mul_setup\n"
        "\tclr r5\n"
        "\n"
        ":i_check\n"
        "\tld r20, :i_body\n"
        "\tbrgt r20, r3, r5\n"
        "\tpriv r0, r0, r0, 0\n"
        "\n"
        ":i_body\n"
        "\tclr r6\n"
        "\n"
        ":j_check\n"
        "\tld r20, :j_body\n"
        "\tbrgt r20, r3, r6\n"
        "\taddi r5, 1\n"
        "\tld r20, :i_check\n"
        "\tbr r20\n"
        "\n"
        ":j_body\n"
        "\tclr r7\n"
        "\tclr r8\n"
        "\n"
        ":k_check\n"
        "\tld r20, :k_body\n"
        "\tbrgt r20, r3, r7\n"
        "\tout r2, r8\n"
        "\taddi r6, 1\n"
        "\tld r20, :j_check\n"
        "\tbr r20\n"
        "\n"
        ":k_body\n"
        "\tmul r11, r5, r3\n"
        "\tadd r11, r11, r7\n"
        "\tshftli r11, 3\n"
        "\tadd r11, r15, r11\n"
        "\tmov r12, (r11)(0)\n"
        "\n"
        "\tmul r13, r7, r3\n"
        "\tadd r13, r13, r6\n"
        "\tshftli r13, 3\n"
        "\tadd r13, r16, r13\n"
        "\tmov r14, (r13)(0)\n"
        "\n"
        "\tmulf r12, r12, r14\n"
        "\taddf r8, r8, r12\n"
        "\n"
        "\taddi r7, 1\n"
        "\tld r20, :k_check\n"
        "\tbr r20\n";

    const char *tkPath = "tmp_mat.tk";
    const char *tkoPath = "tmp_mat.tko";
    const char *inPath = "tmp_in.txt";
    const char *outPath = "tmp_out.txt";

    uint64_t aBits;
    uint64_t bBits;
    uint64_t expectedBits;

    char input[256];
    char expected[256];
    int rc;
    char *out;

    rc = assembleFile(tkPath, tkoPath, tk);
    if (!expectEqIntAt(__FILE__, __LINE__, rc, 0, "assembler rc", "0"))
    {
        return false;
    }

    aBits = doubleBits(2.0);
    bBits = doubleBits(3.0);
    expectedBits = doubleBits(6.0);

    snprintf(input, sizeof(input), "1 %llu %llu\n",
             (unsigned long long)aBits, (unsigned long long)bBits);

    snprintf(expected, sizeof(expected), "%llu\n", (unsigned long long)expectedBits);

    out = runSimulatorCapture(tkoPath, inPath, outPath, input);
    if (!expectStrEqAt(__FILE__, __LINE__, out, expected))
    {
        free(out);
        return false;
    }

    free(out);
    return true;
}

static void runTestSuite(const TestCase *tests, int testCount)
{
    int i;

    i = 0;
    while (i < testCount)
    {
        bool ok;

        g_stats.total += 1;
        g_stats.currentFailed = false;

        printf("Running: %s\n", tests[i].name);

        ok = tests[i].fn();
        if (ok == false || g_stats.currentFailed == true)
        {
            g_stats.failed += 1;
            printf("Result: FAIL\n\n");
        }
        else
        {
            printf("Result: PASS\n\n");
        }

        i += 1;
    }
}

int main(void)
{
    TestCase tests[3];

    memset(&g_stats, 0, sizeof(g_stats));

    tests[0].name = "integration_binary_search_found_and_not_found";
    tests[0].fn = testIntegrationBinarySearch;

    tests[1].name = "integration_fibonacci";
    tests[1].fn = testIntegrationFibonacci;

    tests[2].name = "integration_matrix_mul_n1";
    tests[2].fn = testIntegrationMatrixMulN1;

    printf("HW5 Tests (integration)\n\n");
    runTestSuite(tests, 3);

    printf("Tests run: %d\n", g_stats.total);
    printf("Failed:    %d\n", g_stats.failed);
    printf("Passed:    %d\n", g_stats.total - g_stats.failed);

    if (g_stats.failed == 0)
    {
        return 0;
    }

    return 1;
}