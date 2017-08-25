RIOT_EDBG = $(RIOTBASE)/dist/tools/edbg/edbg
EDBG ?= $(RIOT_EDBG)
FLASHER ?= $(EDBG)
OFLAGS ?= -O binary
HEXFILE = $(ELFFILE:.elf=.bin)
ifeq ($(SERIAL_NUMBER),)
  FFLAGS ?= $(EDBG_ARGS) -t $(EDBG_DEVICE_TYPE) -b -e -v -p -f $(HEXFILE)
else
  FFLAGS ?= $(EDBG_ARGS) -t $(EDBG_DEVICE_TYPE) -b -e -v -p -f $(HEXFILE) -s $(SERIAL_NUMBER)
endif

ifeq ($(RIOT_EDBG),$(FLASHER))
  FLASHDEPS += $(RIOT_EDBG)
endif
