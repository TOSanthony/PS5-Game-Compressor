# Experimental: build a tiny eboot.bin that asks the Game Compressor payload to
# open its web UI in the full system web browser.
#
# Uses the linker script and the fake-self tool shipped with ps5-payload-sdk
# (samples/install_app), so nothing from the SDK samples is copied here.
#
#   export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
#   make -C tools/fullscreen-launcher

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK is undefined)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

SAMPLE   := $(PS5_PAYLOAD_SDK)/samples/install_app
GC_PORT  ?= 5910
OUT      := out

# Values used by the SDK's install_app sample for a fake-signed application.
PS4_SDK_VERSION ?= 0x07590001
AUTHID   := 0x3800000000000022
AUTHINFO := 00 00 00 00 00 00 00 00 00 00 00 00 00 1C 00 40 00 FF 00 00 00 00 \
            00 80 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 80 \
            00 40 00 40 00 00 00 00 00 00 00 80 00 00 00 00 00 00 00 08 00 40 \
            FF FF 00 00 00 F0 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
            00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
            00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 \
            00 00 00 00

all: $(OUT)/eboot.bin

$(OUT):
	mkdir -p $@

$(OUT)/eboot.o: eboot.c | $(OUT)
	$(CC) -c -Os -Wall -Werror -ffreestanding -fno-stack-protector \
	      -DGC_PORT=$(GC_PORT) -o $@ $<

$(OUT)/eboot.elf: $(OUT)/eboot.o
	$(LD) --static -T $(SAMPLE)/eboot.x -o $@ $<

$(OUT)/eboot.bin: $(OUT)/eboot.elf
	python3 $(SAMPLE)/make_fself.py --ptype fake --paid $(AUTHID) \
	    --auth-info "$(AUTHINFO)" \
	    --app-version $(PS4_SDK_VERSION) \
	    --fw-version $(PS4_SDK_VERSION) $< $@

clean:
	rm -rf $(OUT)

.PHONY: all clean
