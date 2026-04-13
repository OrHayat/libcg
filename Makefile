CC       = clang
CFLAGS   = -std=c11 -Wall -Wextra -Wpedantic -O2 -Isrc
BUILD    = build

C_SRCS   = $(shell find src -name '*.c')
C_OBJS   = $(patsubst src/%.c,$(BUILD)/%.o,$(C_SRCS))

TARGET   = $(BUILD)/libcg

all: $(TARGET)

$(TARGET): $(C_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(C_OBJS) -o $@

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

run: all
	./$(TARGET)

.PHONY: all clean run
