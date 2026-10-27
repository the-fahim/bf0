#define _DEFAULT_SOURCE
#include "bfjit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#define BF_NATIVE_JIT 1
#else
#define BF_NATIVE_JIT 0
#endif
#define TAPE_SIZE 10000000u

void bf_free(BfProgram *program)
{
    free(program->commands);
    free(program->jumps);
    *program = (BfProgram){0};
}

bool bf_load(const char *path, BfProgram *program)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return false; }
    BfProgram parsed = {0};
    size_t capacity = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (!ch || !strchr("+-<>.,[]", ch)) continue;
        if (parsed.count == capacity) {
            if (capacity > SIZE_MAX / 2) goto fail;
            size_t next = capacity ? capacity * 2 : 256;
            char *commands = realloc(parsed.commands, next);
            if (!commands) goto fail;
            parsed.commands = commands;
            capacity = next;
        }
        parsed.commands[parsed.count++] = (char)ch;
    }
    if (ferror(file)) { perror(path); goto fail; }
    fclose(file);
    file = NULL;
    if (parsed.count > SIZE_MAX / sizeof(size_t)) goto fail;
    if (parsed.count) {
        parsed.jumps = calloc(parsed.count, sizeof(size_t));
        size_t *stack = malloc(parsed.count * sizeof(size_t));
        if (!parsed.jumps || !stack) { free(stack); goto fail; }
        size_t depth = 0;
        for (size_t i = 0; i < parsed.count; ++i) {
            if (parsed.commands[i] == '[') stack[depth++] = i;
            else if (parsed.commands[i] == ']') {
                if (!depth) {
                    fprintf(stderr, "%s: unmatched ']' at command %zu\n", path, i + 1);
                    free(stack);
                    goto fail;
                }
                size_t open = stack[--depth];
                parsed.jumps[open] = i;
                parsed.jumps[i] = open;
            }
        }
        free(stack);
        if (depth) { fprintf(stderr, "%s: unmatched '['\n", path); goto fail; }
    }
    bf_free(program);
    *program = parsed;
    return true;
fail:
    if (file) fclose(file);
    bf_free(&parsed);
    fprintf(stderr, "Could not load Brainfuck program: %s\n", path);
    return false;
}

bool bf_interpret(const BfProgram *program)
{
    unsigned char *tape = calloc(TAPE_SIZE, 1);
    if (!tape) { perror("tape allocation"); return false; }
    size_t head = 0;
    bool ok = true;
    for (size_t ip = 0; ip < program->count; ++ip) {
        switch (program->commands[ip]) {
        case '+': ++tape[head]; break;
        case '-': --tape[head]; break;
        case '>': if (head == TAPE_SIZE - 1) goto bounds; ++head; break;
        case '<': if (!head) goto bounds; --head; break;
        case '.': if (putchar(tape[head]) == EOF) goto io_error; break;
        case ',': {
            int ch = getchar();
            if (ch != EOF) tape[head] = (unsigned char)ch;
            else if (ferror(stdin)) goto io_error;
        } break;
        case '[': if (!tape[head]) ip = program->jumps[ip]; break;
        case ']': if (tape[head]) ip = program->jumps[ip]; break;
        }
    }
    if (fflush(stdout) == EOF) goto io_error;
    goto done;
bounds:
    fprintf(stderr, "Runtime error: tape pointer out of bounds\n");
    ok = false;
    goto done;
io_error:
    fprintf(stderr, "Runtime error: input/output failed\n");
    ok = false;
done:
    free(tape);
    return ok;
}

bool bf_jit_available(void) { return BF_NATIVE_JIT != 0; }

#if BF_NATIVE_JIT
/* Fixed slots simplify branch patching. System V arguments: rdi = head,
   rsi = tape start, rdx = last cell. r8/r9 preserve bounds across syscalls. */
#define SLOT_SIZE 64u
static void emit32(unsigned char *dst, int32_t value)
{
    memcpy(dst, &value, sizeof(value));
}

bool bf_jit_run(const BfProgram *program)
{
    if (program->count > (INT32_MAX - 16u) / SLOT_SIZE) {
        fprintf(stderr, "Program is too large for the JIT\n");
        return false;
    }
    size_t end = 6 + program->count * SLOT_SIZE;
    size_t length = end + 9;
    unsigned char *code = mmap(NULL, length, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return false; }
    memset(code, 0x90, length);
    memcpy(code, "\x49\x89\xf0\x49\x89\xd1", 6);
    for (size_t i = 0; i < program->count; ++i) {
        size_t offset = 6 + i * SLOT_SIZE;
        unsigned char *p = code + offset;
        switch (program->commands[i]) {
        case '+': memcpy(p, "\xfe\x07", 2); break;
        case '-': memcpy(p, "\xfe\x0f", 2); break;
        case '<':
        case '>': {
            bool left = program->commands[i] == '<';
            memcpy(p, left ? "\x4c\x39\xc7" : "\x4c\x39\xcf", 3);
            memcpy(p + 3, "\x0f\x84", 2);
            emit32(p + 5, (int32_t)(end + 3) - (int32_t)(offset + 9));
            memcpy(p + 9, left ? "\x48\xff\xcf" : "\x48\xff\xc7", 3);
        } break;
        case '[':
        case ']': {
            memcpy(p, "\x80\x3f\x00\x0f", 4);
            p[4] = program->commands[i] == '[' ? 0x84 : 0x85;
            size_t target = 6 + (program->jumps[i] + 1) * SLOT_SIZE;
            emit32(p + 5, (int32_t)target - (int32_t)(offset + 9));
        } break;
        case '.':
        case ',': {
            bool output = program->commands[i] == '.';
            /* push rdi; mov rsi,rdi; mov eax,fd; mov edi,eax;
               mov edx,1; syscall; pop rdi; test rax,rax; js error */
            memcpy(p, "\x57\x48\x89\xfe\xb8", 5);
            emit32(p + 5, output ? 1 : 0);
            memcpy(p + 9, "\x89\xc7\xba\x01\x00\x00\x00\x0f\x05\x5f\x48\x85\xc0\x0f\x88", 15);
            emit32(p + 24, (int32_t)(end + 3) - (int32_t)(offset + 28));
        } break;
        }
    }
    /* Return 0 on success, 1 on a bounds or syscall error. */
    memcpy(code + end, "\x31\xc0\xc3\xb8\x01\x00\x00\x00\xc3", 9);
    if (mprotect(code, length, PROT_READ | PROT_EXEC)) {
        perror("mprotect"); munmap(code, length); return false;
    }
    unsigned char *tape = calloc(TAPE_SIZE, 1);
    if (!tape) { perror("tape allocation"); munmap(code, length); return false; }
    int (*run)(void *, void *, void *);
    _Static_assert(sizeof(run) == sizeof(code), "Unsupported function pointer size");
    memcpy(&run, &code, sizeof(run));
    int status = run(tape, tape, tape + TAPE_SIZE - 1);
    free(tape);
    munmap(code, length);
    if (status) fprintf(stderr, "Runtime error: tape bounds or input/output failure\n");
    return status == 0;
}
#else
bool bf_jit_run(const BfProgram *program)
{
    (void)program;
    fprintf(stderr, "Native JIT requires Linux x86-64\n");
    return false;
}
#endif
