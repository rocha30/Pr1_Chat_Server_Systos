CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
TARGET  = cliente
SRC     = client.c

$(TARGET): $(SRC) ../Protocolo.h
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

.PHONY: clean
