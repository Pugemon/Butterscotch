#---------------------------------------------------------------------------------
# Example 03: Rotating Cube
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

include $(DEVKITARM)/3ds_rules

TARGET   := 03_rotating_cube
SOURCES  := .
BUILD    := build

APP_TITLE    := gl2citro3d Rotating Cube
APP_AUTHOR   := gl2citro3d
APP_DESC     := Rotating textured cube example

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   := -g -Wall -O2 -mword-relocations \
            -ffunction-sections -fdata-sections \
            $(ARCH)

CFLAGS   += -I$(DEVKITPRO)/libctru/include \
            -I../../include

LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map

LIBS     := -L../../src -lgl2citro3d -lcitro3d -lctru -lm

.PHONY: all clean

all: $(BUILD) $(TARGET).3dsx

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/main.o: main.c | $(BUILD)
	@echo "  CC  $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(BUILD)/main.o
	@echo "  LD  $(notdir $@)"
	@$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(TARGET).3dsx: $(TARGET).elf
	@echo "  3DSX $(notdir $@)"
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESC)" "$(APP_AUTHOR)" icon.smdh 2>/dev/null || true
	@3dsxtool $< $@

clean:
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx *.smdh
