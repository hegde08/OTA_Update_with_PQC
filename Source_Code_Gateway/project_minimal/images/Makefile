ROOT_DIR := $(notdir $(CURDIR))
ifndef QCONFIG
QCONFIG=qconfig.mk
endif
include $(QCONFIG)

HOST_MKIFS     := mkifs
HOST_MKQNX6FS  := mkqnx6fsimg
HOST_MKI       := mki
HOST_SED       := sed
SUFFIXES       := .build .bin .raw .ui .img

IMAGE_LOAD_ADDR = 0x80000
PROCESSOR = aarch64
BOARD     = rpi4
INSTALL   = ../install
BUILD_DIR = build

.PHONY: all clean

all: $(BUILD_DIR)/ifs-$(BOARD).bin $(BUILD_DIR)/ifs-$(BOARD).ui $(BUILD_DIR)/rootfs.img

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# 1. Build Minimal OS IFS image
$(BUILD_DIR)/ifs-$(BOARD).bin: $(BOARD).build | $(BUILD_DIR)
	$(HOST_MKIFS) -v -r$(INSTALL) $(MKIFSFLAGS) $< $@

# 2. Wrap IFS image with U-Boot header (ifs-rpi4.ui)
$(BUILD_DIR)/ifs-$(BOARD).ui: $(BUILD_DIR)/ifs-$(BOARD).bin | $(BUILD_DIR)
	$(HOST_MKI) -a $(IMAGE_LOAD_ADDR) -A arm64 $< $@

# 3. Build QNX6 Root Filesystem image (rootfs.img)
$(BUILD_DIR)/rootfs.img: rootfs.build | $(BUILD_DIR)
	PROCESSOR=aarch64le PROCESSOR_BASE=aarch64 $(HOST_MKQNX6FS) -vvv $< $@

clean:
	$(RM_HOST) -rf $(BUILD_DIR) *.sym