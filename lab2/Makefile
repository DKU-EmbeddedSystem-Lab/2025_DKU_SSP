# Compiler and compiler flags
CC = gcc
CFLAGS = -Wall -g -I. -Isim/ -Invme/
CPPFLAGS += -DCOSMOS_SIM=1

# Target executable name
TARGET = cosmos_sim
TEST_TARGETS = tests/test_sim_random tests/test_sim_random_parser tests/test_sim_nand_handoff

# Find all .c source files in the current directory and subdirectories
SRCS = $(wildcard *.c) $(wildcard sim/*.c) $(wildcard nvme/*.c)

# Generate object file names from source file names
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

# The default target: build the executable
all: $(TARGET)

test: $(TARGET) $(TEST_TARGETS)
	./tests/test_sim_random
	./tests/test_sim_random_parser
	timeout 30s ./tests/test_sim_nand_handoff
	sh tests/test_cli.sh ./$(TARGET)
	sh tests/test_random.sh ./$(TARGET)
	sh tests/test_progress.sh ./$(TARGET)

tests/test_sim_random: tests/test_sim_random.c sim/sim_host.c $(wildcard *.h sim/*.h nvme/*.h) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ tests/test_sim_random.c sim/sim_host.c

tests/test_sim_random_parser: tests/test_sim_random_parser.c sim/sim_fio.c sim/sim_fio.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_sim_random_parser.c sim/sim_fio.c

tests/test_sim_nand_handoff: tests/test_sim_nand_handoff.c $(filter-out main.o,$(OBJS)) $(wildcard *.h sim/*.h nvme/*.h) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_sim_nand_handoff.c $(filter-out main.o,$(OBJS))

# Rule to link the object files into the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Rule to compile a .c source file into a .o object file
%.o: %.c Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Rule to clean up the build artifacts
clean:
	rm -f $(TARGET) $(OBJS) $(DEPS) $(TEST_TARGETS)

-include $(DEPS)

.PHONY: all test clean
