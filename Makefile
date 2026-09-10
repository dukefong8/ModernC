######################### Preamble ###########################################
SHELL := bash
.ONESHELL:
.SHELLFLAGS := -eu -o pipefail -c
.DELETE_ON_ERROR:
.SECONDEXPANSION:
MAKEFLAGS += --warn-undefined-variables
MAKEFLAGS += --no-builtin-rules

######################### Project Settings ###################################
.PHONY: all
all: debug

NAME = cmd
BUILD_DIR := ./build
BIN_TARGET = $(BUILD_DIR)/$(NAME)
LIB_TARGET = $(BUILD_DIR)/lib$(NAME).a

SRC := $(shell find . -name '*.c')
LIB_SRC := $(shell find . -name '*.c' -not -name $(NAME).c)
OBJ := $(SRC:%.c=$(BUILD_DIR)/%.o)
LIB_OBJ := $(LIB_SRC:%.c=$(BUILD_DIR)/%.o)
DEP := $(OBJ:.o=.d)

WARN = -Wall -Wextra -Wnull-dereference -Wvla -Wformat=2 -Wno-format-nonliteral -Wno-unused-parameter -Wno-unused-function
SANZ = -fno-common -fno-omit-frame-pointer -fsanitize-trap=unreachable -fsanitize=address,undefined

CPPFLAGS += -I./include -D_GNU_SOURCE -DDEFAULT_ARENA_SIZE=4000000000
CFLAGS   += -MMD -MP $(WARN)
LDFLAGS  += -lm

.PHONY: debug release
debug: CFLAGS += $(SANZ) -O0 -g3 -DLOGGING -DOOM_COMMIT
debug: LDFLAGS += $(SANZ)
debug: $(BIN_TARGET)

release: CFLAGS  += -O2 -g -DNDEBUG -DOOM_COMMIT
release: LDFLAGS +=
release: $(BIN_TARGET)

$(BIN_TARGET): $(OBJ)
	$(CC) -o $@ $(LDFLAGS) $^

$(LIB_TARGET): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o : %.c
	mkdir -p $(dir $@)
	$(CC) -o $@ $(CPPFLAGS) $(CFLAGS) -c $<

-include $(DEP)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

.PHONY: deps
deps:
	(cd include; ../pkg.sh import)
	curl -s --output-dir include -O https://raw.githubusercontent.com/hirrolot/datatype99/refs/heads/master/datatype99.h
	curl -s --output-dir include -O https://raw.githubusercontent.com/hirrolot/interface99/refs/heads/master/interface99.h
	curl -s --output-dir include -O https://raw.githubusercontent.com/attractivechaos/klib/refs/heads/master/ketopt.h
	curl -s --output-dir include -O https://raw.githubusercontent.com/sheredom/utf8.h/refs/heads/master/utf8.h
	curl -s --output-dir include -O https://raw.githubusercontent.com/JacksonAllan/Verstable/refs/heads/main/verstable.h
	curl -s --output-dir include -O https://raw.githubusercontent.com/spievniev/uprintf/refs/heads/main/uprintf.h
	curl -s --output-dir include -O https://raw.githubusercontent.com/sheredom/utest.h/refs/heads/main/utest.h

.PHONY: watch
watch:
	find . -name '*.c' -o -name '*.h' | entr -cc clang $(WARN) $(CPPFLAGS) -fsyntax-only -ferror-limit=1 -fmacro-backtrace-limit=1 /_
