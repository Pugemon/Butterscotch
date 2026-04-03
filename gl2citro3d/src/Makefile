#---------------------------------------------------------------------------------
# gl2citro3d static library build
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/3ds_rules

TARGET   := libgl2citro3d
SOURCES  := .
BUILD    := build

CFLAGS   := -g -Wall -O2 -mword-relocations \
            -ffunction-sections -fdata-sections \
            $(ARCH) $(INCLUDE)

CFLAGS   += -I$(DEVKITPRO)/libctru/include \
            -I$(DEVKITPRO)/portlibs/3ds/include \
            -I../include

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

#---------------------------------------------------------------------------------
# Build the shader binary from .pica source
#---------------------------------------------------------------------------------
SHADERS_DIR := ../shaders
SHADER_SRC  := $(SHADERS_DIR)/gl2citro3d_shader.pica
SHADER_BIN  := $(BUILD)/gl2citro3d_shader.shbin
SHADER_H    := $(BUILD)/gl2citro3d_shader_shbin.h

#---------------------------------------------------------------------------------

OFILES  := $(BUILD)/gl2citro3d.o

.PHONY: all clean

all: $(BUILD) $(TARGET).a

$(BUILD):
	@mkdir -p $(BUILD)

$(SHADER_BIN): $(SHADER_SRC) | $(BUILD)
	@echo "  PICASSO  $(notdir $<)"
	@picasso -o $@ $<

$(SHADER_H): $(SHADER_BIN)
	@echo "  BIN2S    $(notdir $<)"
	@bin2s $< | $(AS) -o $(BUILD)/shader.o
	@echo "extern const u8 gl2citro3d_shader_shbin[];" > $@
	@echo "extern const u32 gl2citro3d_shader_shbin_size;" >> $@

$(BUILD)/gl2citro3d.o: gl2citro3d.c $(SHADER_H) | $(BUILD)
	@echo "  CC       $(notdir $<)"
	@$(CC) $(CFLAGS) -I$(BUILD) -c $< -o $@

$(TARGET).a: $(OFILES) $(BUILD)/shader.o
	@echo "  AR       $(notdir $@)"
	@$(AR) rcs $@ $^

clean:
	@rm -rf $(BUILD) $(TARGET).a
