.DEFAULT_GOAL := all
PROFILE ?= 64k
ifneq ($(filter $(PROFILE),64k 224k),$(PROFILE))
$(error PROFILE must be 64k or 224k)
endif
CROSS_COMPILE ?= $(WCH_TOOLCHAIN_PATH)riscv-wch-elf-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE := $(CROSS_COMPILE)size
BUILD := build/$(PROFILE)
TARGET := $(BUILD)/badapple
FLASH_SIZE := $(if $(filter 224k,$(PROFILE)),229376,65536)
ARCH := -march=rv32imacxw -mabi=ilp32
CFLAGS := $(ARCH) -Os -flto -ffunction-sections -fdata-sections -fno-common \
          -msmall-data-limit=8 -msave-restore -std=gnu17 -Wall -Wextra \
          -Wno-unused-function -Isrc -Ihal \
          -DAPPLE_WFI=$(if $(filter 224k,$(PROFILE)),1,0) \
          -DAPPLE_DATA_HEADER=\"$(abspath $(PROFILE)/apple_data.h)\"
SOURCES := $(wildcard src/*.c) $(wildcard hal/*.c)
OBJECTS := $(addprefix $(BUILD)/,$(SOURCES:.c=.o)) $(BUILD)/hal/startup.o

.PHONY: all 64k 224k firmware size clean flash
all: 64k 224k
64k 224k:
	$(MAKE) PROFILE=$@ firmware
firmware: $(TARGET).bin $(TARGET).hex size
$(BUILD)/%.o: %.c Makefile
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/hal/startup.o: hal/startup.S Makefile
	mkdir -p $(@D)
	$(CC) $(ARCH) -msave-restore -c $< -o $@
$(TARGET).elf: $(OBJECTS) Link.ld Makefile
	$(CC) $(CFLAGS) -nostartfiles --specs=nano.specs --specs=nosys.specs \
	  -Wl,--defsym=__flash_size=$(FLASH_SIZE) -T Link.ld \
	  -Wl,--gc-sections,--print-memory-usage,-Map,$(TARGET).map $(OBJECTS) -o $@
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary -S $< $@
$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex $< $@
size: $(TARGET).elf
	$(SIZE) $<
MINICHLINK ?= ./minichlink
flash: firmware
	$(MINICHLINK) -w $(TARGET).bin flash -b
clean:
	rm -rf build
-include $(OBJECTS:.o=.d)
