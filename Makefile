#   etaHEN Game Mounter - Auto mount games from /data/etaHEN/games/
#   Based on dump_runner by John Törnblom

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

TARGET := game_mounter.elf

CFLAGS := -Wall -Werror -lSceSystemService -lSceUserService -lSceAppInstUtil

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

test: $(TARGET)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
