ifndef QNX_TARGET
$(error QNX_TARGET is not set. Run 'source <path-to-qnx>/qnxsdp-env.sh' or '<path-to-qnx>\qnxsdp-env.bat' first)
endif

PLATFORM      ?= aarch64le
BUILD_PROFILE ?= release

APP_DIRS = download_agent verifier writer
BSP_DIR  = project_minimal

.PHONY: all apps bsp clean rebuild $(APP_DIRS) $(BSP_DIR)

# 1. Build the Gateway applications first, then package the BSP & rootfs.img
all: apps $(BSP_DIR)

apps: $(APP_DIRS)

$(APP_DIRS):
	$(MAKE) -C $@ PLATFORM=$(PLATFORM) BUILD_PROFILE=$(BUILD_PROFILE)

# 2. BSP depends on apps so Verifier, Writer, and download_agent binaries are ready for rootfs.build
$(BSP_DIR): apps
	$(MAKE) -C $@

clean:
	@for dir in $(APP_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	$(MAKE) -C $(BSP_DIR) clean

rebuild: clean all