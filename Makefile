CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS =
LDLIBS =

ifeq ($(OS),Windows_NT)
EXEEXT = .exe
endif

TARGET = bfjit$(EXEEXT)

.PHONY: all run clean
all: $(TARGET)

$(TARGET): main.c bfjit.c bfjit.h
	$(CC) $(CFLAGS) main.c bfjit.c $(LDFLAGS) $(LDLIBS) -o $@

# Example: make run FILE=hello.bf ARGS=--no-jit
run: $(TARGET)
	./$(TARGET) $(ARGS) "$(FILE)"

clean:
	$(RM) bfjit bfjit.exe
