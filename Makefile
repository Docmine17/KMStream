CC       ?= gcc
PKG_CFG  ?= pkg-config
ARCH     := $(shell uname -m)

CFLAGS   ?= -O2 -pipe -flto
CFLAGS   += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Icommon -Iserver -MMD -MP
LDFLAGS  ?= -flto

ifeq ($(ARCH),x86_64)
  CFLAGS += -march=native
  DEFAULT_TARGET := server
else ifeq ($(ARCH),aarch64)
  CFLAGS += -mcpu=cortex-a53
  DEFAULT_TARGET := client
else
  DEFAULT_TARGET := client
endif

BUILD_DIR  := build
SERVER_BIN := $(BUILD_DIR)/kmstreamd
CLIENT_BIN := $(BUILD_DIR)/kmstream-pi

SERVER_PKGS := libdrm gbm egl glesv2 libva libva-drm libavcodec libavutil
CLIENT_PKGS := libdrm libavcodec libavutil

SERVER_SRCS := server/kmstreamd.c server/sps_patch.c
SERVER_OBJS := $(patsubst server/%.c,$(BUILD_DIR)/server/%.o,$(SERVER_SRCS))

CLIENT_SRCS := client/kmstream_pi.c
CLIENT_OBJS := $(patsubst client/%.c,$(BUILD_DIR)/client/%.o,$(CLIENT_SRCS))

.PHONY: all server client debug caps clean

SERVER_CFLAGS = $(shell $(PKG_CFG) --cflags $(SERVER_PKGS))
SERVER_LIBS   = $(shell $(PKG_CFG) --libs $(SERVER_PKGS))
CLIENT_CFLAGS = $(shell $(PKG_CFG) --cflags $(CLIENT_PKGS))
CLIENT_LIBS   = $(shell $(PKG_CFG) --libs $(CLIENT_PKGS))

all: $(DEFAULT_TARGET)

server: $(SERVER_BIN)

client: $(CLIENT_BIN)

$(BUILD_DIR)/server/%.o: server/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(LDFLAGS) $^ $(SERVER_LIBS) -o $@

$(BUILD_DIR)/client/%.o: client/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -c $< -o $@

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(LDFLAGS) $^ $(CLIENT_LIBS) -o $@

debug: CFLAGS := -O0 -g3 -fsanitize=address,undefined -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Icommon -Iserver -MMD -MP
debug: LDFLAGS := -fsanitize=address,undefined
debug: $(DEFAULT_TARGET)

caps: $(SERVER_BIN)
	sudo setcap cap_sys_admin+ep $(SERVER_BIN)

clean:
	rm -rf $(BUILD_DIR)

-include $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d)
