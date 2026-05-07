CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -Iinclude -g
LDFLAGS = -lncurses

FOSSIL_HASH := $(shell fossil info 2>/dev/null | grep '^checkout:' | awk '{print substr($$2,1,12)}')
ifeq ($(FOSSIL_HASH),)
FOSSIL_HASH := unknown
endif
CFLAGS += -DMEI_BUILD_HASH=\"$(FOSSIL_HASH)\"

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

TARGET = $(BIN_DIR)/orchestrator_ui

# Find all C files recursively in src directory
SRCS = $(shell find $(SRC_DIR) -name '*.c')
# Map src/*.c to obj/*.o preserving directory structure inside obj/
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/ui $(OBJ_DIR)/utils $(OBJ_DIR)/cmd

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

install: all
	@mkdir -p $(HOME)/bin
	cp $(TARGET) $(HOME)/bin/mei

.PHONY: all dirs clean install
