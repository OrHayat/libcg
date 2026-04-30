CC       = clang
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -O2 -Isrc
BUILD    = build

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    PLATFORM_SRC = src/platform/platform_macos.m
    LDFLAGS      = -framework Cocoa -framework QuartzCore -framework IOKit
    OBJCFLAGS    = -fobjc-arc
endif

C_SRCS   = $(shell find src -name '*.c')

C_OBJS   = $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRCS))
M_OBJS   = $(patsubst src/%.m,$(BUILD)/%.o,$(filter %.m,$(PLATFORM_SRC)))
ALL_OBJS = $(C_OBJS) $(M_OBJS)

TARGET   = $(BUILD)/libcg

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_OBJS) -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.m
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(OBJCFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

run: all
	./$(TARGET)

# Regenerate compile_commands.json for clangd (IDE completion / go-to-def).
# Requires `bear` (brew install bear). Intercepts a clean build to record flags.
compile_commands.json:
	$(MAKE) clean
	bear -- $(MAKE) all

compdb: compile_commands.json

.PHONY: all clean run compdb
