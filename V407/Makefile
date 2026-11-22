TARGET := appleEPD407
BUILD_DIR ?= build/firmware
OPT ?= -O3
MINICHLINK ?= $(abspath minichlink)

# WCH GCC15 is required.  Accept both common WSL spellings and a compiler on PATH.
WCH407_DEFAULT_TOOLCHAIN_DIRS := \
    $(HOME)/Toolchain/MRS_Toolchain_Linux_X64_V240/Toolchain/wchgcc15/bin \
    $(HOME)/toolchains/MRS_Toolchain_Linux_X64_V240/Toolchain/wchgcc15/bin
WCH407_TOOLCHAIN_PATH ?= $(firstword $(wildcard $(WCH407_DEFAULT_TOOLCHAIN_DIRS)))
ifeq ($(origin PREFIX),undefined)
  ifneq ($(strip $(WCH407_TOOLCHAIN_PATH)),)
    PREFIX := $(WCH407_TOOLCHAIN_PATH)/riscv32-wch-elf-
  else
    WCH407_CC_PATH := $(shell command -v riscv32-wch-elf-gcc 2>/dev/null)
    ifneq ($(strip $(WCH407_CC_PATH)),)
      PREFIX := $(patsubst %riscv32-wch-elf-gcc,%,$(WCH407_CC_PATH))
    else
      WCH407_TOOLCHAIN_PATH := $(firstword $(WCH407_DEFAULT_TOOLCHAIN_DIRS))
      PREFIX := $(WCH407_TOOLCHAIN_PATH)/riscv32-wch-elf-
    endif
  endif
endif

CC := $(PREFIX)gcc
AS := $(PREFIX)gcc
CP := $(PREFIX)objcopy
SZ := $(PREFIX)size
NM := $(PREFIX)nm

ARCH ?= -march=rv32imac_zba_zbb_zbc_zbs_zve64x_zvl64b_zvbb_xw -mabi=ilp32
C_SOURCES := $(wildcard ./*.c) $(wildcard platform/*.c) assets/apple_data.c \
             $(wildcard SRC/Core/*.c) $(wildcard SRC/Debug/*.c) \
             $(wildcard SRC/Peripheral/src/*.c)
ASM_SOURCES := SRC/Startup/startup_ch32v4x7.S
C_INCLUDES := -I. -Iassets -Iplatform -ISRC/Core -ISRC/Debug -ISRC/Peripheral/inc

COMMON_FLAGS := $(ARCH) $(OPT) -g -gdwarf-4 -msmall-data-limit=8 -msave-restore \
                -fsigned-char -ffunction-sections -fdata-sections -fno-common
VECTOR_FLAGS := -fno-tree-loop-vectorize -fno-tree-slp-vectorize -mrvv-vector-bits=zvl
CPPFLAGS = $(C_INCLUDES) -include app_config.h
CFLAGS = $(COMMON_FLAGS) -std=gnu17 -Wall -Wextra -Wno-unused-parameter \
         $(VECTOR_FLAGS) $(CPPFLAGS)
ASFLAGS = $(COMMON_FLAGS) -x assembler-with-cpp $(CPPFLAGS)
DEPFLAGS = -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)"
LDFLAGS := $(COMMON_FLAGS) -TLink.ld -nostartfiles \
           --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections,--print-memory-usage,-Map,$(BUILD_DIR)/$(TARGET).map
LIBS := -lc -lm

OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.S=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
vpath %.S $(sort $(dir $(ASM_SOURCES)))

ELF := $(BUILD_DIR)/$(TARGET).elf
HEX := $(BUILD_DIR)/$(TARGET).hex
BIN := $(BUILD_DIR)/$(TARGET).bin

APPLE_PAYLOAD_BYTES := $(shell sed -n 's/^#define APPLE_PAYLOAD_SIZE[[:space:]]*\([0-9][0-9]*\)u.*/\1/p' assets/apple_data.h)
APPLE_GLYPH_COUNT := $(shell sed -n 's/^#define APPLE_GLYPH_COUNT[[:space:]]*\([0-9][0-9]*\)u.*/\1/p' assets/apple_data.h)
ifeq ($(strip $(APPLE_PAYLOAD_BYTES)),)
$(error Unable to read APPLE_PAYLOAD_SIZE from assets/apple_data.h)
endif
ifeq ($(strip $(APPLE_GLYPH_COUNT)),)
$(error Unable to read APPLE_GLYPH_COUNT from assets/apple_data.h)
endif
APPLE_GLYPH_BYTES := $(shell expr $(APPLE_GLYPH_COUNT) \* 8)

all: $(ELF) $(HEX) $(BIN) verify-assets
build: $(BIN) verify-assets

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	@echo "CC $<"
	@$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.S Makefile | $(BUILD_DIR)
	@echo "AS $<"
	@$(AS) $(ASFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/apple_data.o: assets/apple_data.c assets/apple_data.h

$(ELF): $(OBJECTS) Link.ld Makefile | $(BUILD_DIR)
	@echo "LD $@"
	@$(CC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@
	@$(SZ) $@

$(HEX): $(ELF)
	@$(CP) -O ihex $< $@

$(BIN): $(ELF)
	@$(CP) -O binary -S $< $@

.PHONY: all build clean verify-assets flash recover

flash: $(BIN) verify-assets
	@echo "Flashing $(TARGET) with minichlink"
	@$(MINICHLINK) -u -w "$(abspath $(BIN))" flash -b

recover:
	@echo "Clearing $(TARGET) flash"
	@$(MINICHLINK) -u

verify-assets: $(ELF)
	@set -e; \
	  get_size() { "$(NM)" -S --defined-only "$(ELF)" | awk -v sym="$$1" '$$4 == sym { print "0x" $$2; exit }'; }; \
	  payload=$$(get_size apple_payload); \
	  glyphs=$$(get_size apple_glyphs); \
	  classes=$$(get_size apple_classes); \
	  tree=$$(get_size apple_tree); \
	  run=$$(get_size apple_run); \
	  [ -n "$$payload" ] || { echo "missing apple_payload"; exit 1; }; \
	  [ "$$((payload))" -eq $(APPLE_PAYLOAD_BYTES) ] || { echo "apple_payload size $$((payload)) != $(APPLE_PAYLOAD_BYTES)"; exit 1; }; \
	  [ -n "$$glyphs" ] || { echo "missing apple_glyphs"; exit 1; }; \
	  [ "$$((glyphs))" -eq $(APPLE_GLYPH_BYTES) ] || { echo "apple_glyphs size $$((glyphs)) != $(APPLE_GLYPH_BYTES)"; exit 1; }; \
	  [ -n "$$classes" ] || { echo "missing apple_classes"; exit 1; }; \
	  [ "$$((classes))" -eq $(APPLE_GLYPH_COUNT) ] || { echo "apple_classes size $$((classes)) != $(APPLE_GLYPH_COUNT)"; exit 1; }; \
	  [ -n "$$tree" ] && [ "$$((tree))" -gt 0 ] || { echo "missing/empty apple_tree"; exit 1; }; \
	  [ -n "$$run" ] && [ "$$((run))" -gt 0 ] || { echo "missing/empty apple_run"; exit 1; }; 

clean:
	@rm -rf $(BUILD_DIR)

-include $(OBJECTS:.o=.d)
