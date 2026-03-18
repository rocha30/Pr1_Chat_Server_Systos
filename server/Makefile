CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
TARGET  = servidor
SRC     = server.c

$(TARGET): $(SRC) ../Protocolo.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: clean
