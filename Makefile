CC = gcc
CFLAGS = -Wall -Wextra -O2 -static
TARGET = vimeca
SRC = vimeca.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: all clean
