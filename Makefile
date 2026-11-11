CC       ?= gcc
PKG_CFG  ?= pkg-config
ARCH     := $(shell uname -m)
stats    ?= 0

ifeq ($(stats),1)
  CFLAGS  ?= -O3 -pipe -flto
  CFLAGS  += -DKMSTREAM_STATS=1
  LDFLAGS ?= -flto
else
  CFLAGS  ?= -O3 -pipe -flto -DNDEBUG
  CFLAGS  += -DKMSTREAM_STATS=0
  LDFLAGS ?= -flto -s
endif

CFLAGS   += -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Icommon -Iserver -Iclient -MMD -MP

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
MODE_FILE  := $(BUILD_DIR)/.build_mode
SERVER_BIN := $(BUILD_DIR)/kmstreamd
CLIENT_BIN := $(BUILD_DIR)/kmstream-pi

SERVER_PKGS := libdrm gbm egl glesv2 libva libva-drm libavcodec libavutil
CLIENT_PKGS := libdrm

SERVER_SRCS := server/kmstreamd.c server/sps_patch.c server/capture_kms.c server/convert_egl.c server/encode_vaapi.c server/net_tx.c
SERVER_OBJS := $(patsubst server/%.c,$(BUILD_DIR)/server/%.o,$(SERVER_SRCS))

CLIENT_SRCS := client/kmstream_pi.c client/net_rx.c client/decode_v4l2.c client/display_kms.c
CLIENT_OBJS := $(patsubst client/%.c,$(BUILD_DIR)/client/%.o,$(CLIENT_SRCS))

.PHONY: all server client debug caps clean FORCE

SERVER_CFLAGS = $(shell $(PKG_CFG) --cflags $(SERVER_PKGS))
SERVER_LIBS   = $(shell $(PKG_CFG) --libs $(SERVER_PKGS))
CLIENT_CFLAGS = $(shell $(PKG_CFG) --cflags $(CLIENT_PKGS)) -pthread
CLIENT_LIBS   = $(shell $(PKG_CFG) --libs $(CLIENT_PKGS)) -lpthread

all: $(DEFAULT_TARGET)

server: $(SERVER_BIN)

client: $(CLIENT_BIN)

$(MODE_FILE): FORCE
	@mkdir -p $(BUILD_DIR)
	@echo "stats=$(stats)" > $(MODE_FILE).tmp
	@if ! cmp -s $(MODE_FILE).tmp $(MODE_FILE) 2>/dev/null; then \
		mv $(MODE_FILE).tmp $(MODE_FILE); \
	else \
		rm -f $(MODE_FILE).tmp; \
	fi

$(BUILD_DIR)/server/%.o: server/%.c $(MODE_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SERVER_CFLAGS) -c $< -o $@

$(SERVER_BIN): $(SERVER_OBJS)
	$(CC) $(LDFLAGS) $^ $(SERVER_LIBS) -o $@

$(BUILD_DIR)/client/%.o: client/%.c $(MODE_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CLIENT_CFLAGS) -c $< -o $@

$(CLIENT_BIN): $(CLIENT_OBJS)
	$(CC) $(LDFLAGS) $^ $(CLIENT_LIBS) -o $@

debug: CFLAGS := -O0 -g3 -fsanitize=address,undefined -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Icommon -Iserver -Iclient -DKMSTREAM_STATS=1 -MMD -MP
debug: LDFLAGS := -fsanitize=address,undefined
debug: $(DEFAULT_TARGET)

caps:
	@if [ ! -f $(SERVER_BIN) ]; then $(MAKE) $(SERVER_BIN); fi
	pkexec setcap cap_sys_admin+ep $(SERVER_BIN)

clean:
	rm -rf $(BUILD_DIR)

-include $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d)
