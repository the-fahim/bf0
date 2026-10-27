#ifndef BFJIT_H
#define BFJIT_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *commands;
    size_t *jumps;
    size_t count;
} BfProgram;

/* Initialize programs to {0}; failures report diagnostics to stderr. */
bool bf_load(const char *path, BfProgram *program);
void bf_free(BfProgram *program);
bool bf_interpret(const BfProgram *program);
bool bf_jit_available(void);
bool bf_jit_run(const BfProgram *program);
#endif
