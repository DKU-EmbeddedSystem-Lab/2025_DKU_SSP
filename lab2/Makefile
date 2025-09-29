# Compiler and compiler flags
CC = gcc
CFLAGS = -Wall -g -I. -Isim/ -Invme/

# Target executable name
TARGET = cosmos_sim

# Find all .c source files in the current directory and subdirectories
SRCS = $(wildcard *.c) $(wildcard sim/*.c) $(wildcard nvme/*.c)

# Generate object file names from source file names
OBJS = $(SRCS:.c=.o)

# The default target: build the executable
all: $(TARGET)

# Rule to link the object files into the final executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# Rule to compile a .c source file into a .o object file
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to clean up the build artifacts
clean:
	rm -f $(TARGET) $(OBJS)
