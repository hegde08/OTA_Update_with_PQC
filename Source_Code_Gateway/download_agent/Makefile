# Verify QNX SDP environment is loaded before doing anything
ifndef QNX_TARGET
$(error QNX_TARGET is not set. Run 'source <path-to-qnx>/qnxsdp-env.sh' (Linux/macOS) or '<path-to-qnx>\qnxsdp-env.bat' (Windows) first)
endif

ARTIFACT = download_agent

# Target architecture (Raspberry Pi 4 = aarch64le)
PLATFORM ?= aarch64le

# Build profile: release | debug | profile | coverage
BUILD_PROFILE ?= release

CONFIG_NAME ?= $(PLATFORM)-$(BUILD_PROFILE)
OUTPUT_DIR = build/$(CONFIG_NAME)
TARGET = $(OUTPUT_DIR)/$(ARTIFACT)

# Compiler definitions
CC  = qcc -Vgcc_nto$(PLATFORM)
CXX = q++ -Vgcc_nto$(PLATFORM)_cxx
LD  = $(CXX)

# Include and library paths (relative + QNX_TARGET sysroot)
INCLUDES += -I./inc -I../common/inc -I$(QNX_TARGET)/usr/include
LIBS     += -L$(QNX_TARGET)/$(PLATFORM)/usr/lib -lcurl -lsocket

# Compiler flags for build profiles
CCFLAGS_release  += -O2 -DNDEBUG
CCFLAGS_debug    += -g -O0 -fno-builtin
CCFLAGS_coverage += -g -O0 -ftest-coverage -fprofile-arcs
LDFLAGS_coverage += -ftest-coverage -fprofile-arcs
CCFLAGS_profile  += -g -O0 -finstrument-functions
LIBS_profile     += -lprofilingS

# Generic compiler flags
CCFLAGS_all += -Wall -Wextra -std=gnu++17 -fmessage-length=0 $(CCFLAGS_$(BUILD_PROFILE))
LDFLAGS_all += $(LDFLAGS_$(BUILD_PROFILE))
LIBS_all    += $(LIBS_$(BUILD_PROFILE))
DEPS         = -Wp,-MMD,$(@:%.o=%.d),-MT,$@

# Recursive source discovery
rwildcard = $(wildcard $(addprefix $1/*.,$2)) $(foreach d,$(wildcard $1/*),$(call rwildcard,$d,$2))
SRCS = $(call rwildcard, src, c cpp)
OBJS = $(addprefix $(OUTPUT_DIR)/,$(addsuffix .o, $(basename $(SRCS))))

all: $(TARGET)

$(OUTPUT_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(DEPS) -o $@ $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) $<

$(OUTPUT_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(DEPS) -o $@ $(INCLUDES) $(CCFLAGS_all) $(CCFLAGS) $<

$(TARGET): $(OBJS)
	$(LD) -o $(TARGET) $(LDFLAGS_all) $(LDFLAGS) $(OBJS) $(LIBS_all) $(LIBS)

clean:
	rm -rf build

rebuild: clean all

-include $(OBJS:%.o=%.d)