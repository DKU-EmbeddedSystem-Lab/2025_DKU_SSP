# Compiler and compiler flags
CC = gcc
CFLAGS = -Wall -g -I. -Isim/ -Invme/

# Target executable name
TARGET = cosmos_sim
FIO_TEST = tests/test_sim_fio

# Find all .c source files in the current directory and subdirectories
SRCS = $(wildcard *.c) $(wildcard sim/*.c) $(wildcard nvme/*.c)

# Generate object file names from source file names
OBJS = $(SRCS:.c=.o)

# The default target: build the executable
all: $(TARGET)

test: $(FIO_TEST)
	./$(FIO_TEST) tests/fixtures/valid.fio tests/fixtures/units.fio tests/fixtures/invalid.fio

$(FIO_TEST): tests/test_sim_fio.c sim/sim_fio.c sim/sim_fio.h
	$(CC) $(CFLAGS) -std=c11 -Wextra -Wpedantic -o $@ tests/test_sim_fio.c sim/sim_fio.c

# Rule to link the object files into the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Rule to compile a .c source file into a .o object file
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to clean up the build artifacts
clean:
	rm -f $(TARGET) $(OBJS) $(FIO_TEST)

.PHONY: all test clean
