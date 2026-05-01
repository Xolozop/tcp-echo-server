CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread
DEBUG_FLAGS = -Wall -Wextra -g -pthread -DDEBUG

TARGET1 = echo_server_thread
TARGET2 = echo_server_epoll
TARGET3 = echo_zero_copy
SRC1 = src/lvl_1.c
SRC2 = src/lvl_2.c
SRC3 = src/lvl_3.c

all: $(TARGET1) $(TARGET2) $(TARGET3)

$(TARGET1): $(SRC1)
	$(CC) $(CFLAGS) -o $(TARGET1) $(SRC1)

$(TARGET2): $(SRC2)
	$(CC) $(CFLAGS) -o $(TARGET2) $(SRC2)

$(TARGET3): $(SRC3)
	$(CC) $(CFLAGS) -o $(TARGET3) $(SRC3)

debug:
	$(CC) $(DEBUG_FLAGS) -o $(TARGET1)-debug $(SRC1)
	$(CC) $(DEBUG_FLAGS) -o $(TARGET2)-debug $(SRC2)
	$(CC) $(DEBUG_FLAGS) -o $(TARGET3)-debug $(SRC3)

clean:
	rm -f $(TARGET1) $(TARGET2) $(TARGET3) $(TARGET1)-debug $(TARGET2)-debug $(TARGET3)-debug

run1: $(TARGET1)
	mkdir -p logs
	./$(TARGET1) $(PORT)

run2: $(TARGET2)
	mkdir -p logs
	./$(TARGET2) $(PORT)

run3: $(TARGET3)
	mkdir -p logs
	./$(TARGET3) $(PORT)

PORT ?= 8080

.PHONY: all debug clean run1 run2 run3