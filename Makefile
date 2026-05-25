CC      ?= gcc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic -Wshadow \
           -D_FORTIFY_SOURCE=2 -fstack-protector-strong
LDFLAGS ?= -lm
TARGET  := promo_maker
STB_DIR := stb
STB_HDR := $(STB_DIR)/stb_image.h $(STB_DIR)/stb_image_write.h $(STB_DIR)/stb_truetype.h

.PHONY: all clean stb

all: $(TARGET)

$(TARGET): promo_maker.c $(STB_HDR)
	$(CC) $(CFLAGS) -o $@ promo_maker.c $(LDFLAGS)

stb: $(STB_HDR)

$(STB_DIR)/%.h:
	@mkdir -p $(STB_DIR)
	curl -fsSL -o $@ https://raw.githubusercontent.com/nothings/stb/master/$(*F).h

clean:
	rm -f $(TARGET) $(TARGET).o
