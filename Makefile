CC = gcc
CFLAGS = -Wall -Wextra -O2 -static -g
TARGET = vimeca
SRC = vimeca.c

all: $(TARGET)

$(TARGET): $(SRC) Makefile
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
