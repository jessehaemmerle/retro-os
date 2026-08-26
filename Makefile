# ---------------------------------------------------------------------------
# RetroOS - ein kleines 64-Bit-Betriebssystem mit grafischer Oberflaeche
# ---------------------------------------------------------------------------
#   make            Kernel und bootfaehiges ISO-Image bauen
#   make run        ISO in QEMU starten (BIOS)
#   make run-uefi   ISO in QEMU starten (UEFI, benoetigt OVMF)
#   make clean      Build-Artefakte entfernen
# ---------------------------------------------------------------------------

NAME       := retroos
BUILD      := build
ISO_ROOT   := $(BUILD)/iso_root
KERNEL     := $(BUILD)/$(NAME).elf
ISO        := $(NAME).iso
LIMINE     := third_party/limine

CC         := gcc
LD         := ld
AS         := gcc

CFLAGS := -std=gnu11 -O2 -g \
          -Wall -Wextra -Wno-unused-parameter \
          -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
          -fno-lto -fno-PIC -fno-omit-frame-pointer \
          -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
          -mno-red-zone -mcmodel=kernel \
          -Ikernel/include -I$(LIMINE)

ASFLAGS := $(CFLAGS)

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 \
           -T kernel/linker.ld -m elf_x86_64

CFILES := $(shell find kernel/src -name '*.c' | sort)
SFILES := $(shell find kernel/src -name '*.S' | sort)
OBJS   := $(patsubst kernel/src/%.c,$(BUILD)/obj/%.c.o,$(CFILES)) \
          $(patsubst kernel/src/%.S,$(BUILD)/obj/%.S.o,$(SFILES))
DEPS   := $(OBJS:.o=.d)

.PHONY: all kernel iso run run-uefi clean distclean limine

all: iso

limine:
	@scripts/fetch-limine.sh

kernel: limine $(KERNEL)

$(KERNEL): $(OBJS) kernel/linker.ld
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/obj/%.c.o: kernel/src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.S.o: kernel/src/%.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	@$(AS) $(ASFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

iso: kernel
	@echo "  ISO     $(ISO)"
	@rm -rf $(ISO_ROOT)
	@mkdir -p $(ISO_ROOT)/boot $(ISO_ROOT)/EFI/BOOT
	@cp $(KERNEL) $(ISO_ROOT)/boot/$(NAME).elf
	@cp boot/limine.conf $(ISO_ROOT)/boot/
	@cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	    $(LIMINE)/limine-uefi-cd.bin $(ISO_ROOT)/boot/
	@cp $(LIMINE)/BOOTX64.EFI $(LIMINE)/BOOTIA32.EFI $(ISO_ROOT)/EFI/BOOT/
	@xorriso -as mkisofs -quiet -R -r -J \
	    -b boot/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
	    -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
	    --protective-msdos-label $(ISO_ROOT) -o $(ISO) 2>/dev/null
	@$(LIMINE)/limine bios-install $(ISO) 2>/dev/null
	@echo "  fertig  $(ISO)"

run: iso
	@scripts/run.sh

run-uefi: iso
	@scripts/run.sh --uefi

clean:
	@rm -rf $(BUILD) $(ISO)

distclean: clean
	@rm -rf third_party
