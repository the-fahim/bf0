#include "bfjit.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
    const char *path = NULL;
    bool no_jit = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-jit") == 0) no_jit = true;
        else if (!path && argv[i][0] != '-') path = argv[i];
        else goto usage;
    }
    if (!path) goto usage;
#ifdef _WIN32
    /* Brainfuck input/output operates on bytes, including newline and Ctrl-Z. */
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 ||
        _setmode(_fileno(stdout), _O_BINARY) == -1) {
        perror("binary input/output");
        return 1;
    }
#endif
    BfProgram program = {0};
    if (!bf_load(path, &program)) return 1;
    bool ok = no_jit || !bf_jit_available()
        ? bf_interpret(&program) : bf_jit_run(&program);
    bf_free(&program);
    return ok ? 0 : 1;
usage:
    fprintf(stderr, "Usage: %s [--no-jit] <program.bf|program.f>\n", argv[0]);
    return 1;
}
