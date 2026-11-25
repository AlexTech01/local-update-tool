DEVKITPRO ?= /opt/devkitpro
include $(DEVKITPRO)/libounce/switch2_rules

TARGET := local-update-tool
APP_TITLE := Local Update Tool
APP_AUTHOR := alzxk11
APP_VERSION := 1.0.0
ARCH := $(SWITCH2_CPU) -mtp=soft -fPIE
CFLAGS := -std=gnu11 -g -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections $(ARCH) \
          -Iinclude -I$(LIBNX)/include -I$(PORTLIBS)/include
LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(TARGET).map -Wl,--wrap=smGetServiceWrapper -Wl,--wrap=nsRequestReceiveApplication
LIBS := -lkaihen -lnx -lz
OFILES := $(patsubst source/%.c,build/%.o,$(wildcard source/*.c))
NROFLAGS := --nacp=$(TARGET).nacp --icon=$(LIBNX)/default_icon.jpg

.DEFAULT_GOAL := all
.PHONY: all clean
all: $(TARGET).nro

build:
	mkdir -p $@

build/%.o: source/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(TARGET).elf: $(OFILES)
	$(CC) $(LDFLAGS) $(OFILES) -L$(PORTLIBS_SWITCH2)/lib -L$(LIBNX)/lib -L$(PORTLIBS)/lib $(LIBS) -o $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp

clean:
	rm -rf build $(TARGET).elf $(TARGET).nro $(TARGET).nacp $(TARGET).map

-include $(OFILES:.o=.d)
