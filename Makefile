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

LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
           -T kernel/linker.ld -m elf_x86_64

# --- Benutzerprogramme (laufen in Ring 3) ---------------------------------
UPROGS   := hallo zaehler katze liste schreiben absturz schutz
UDIR     := $(BUILD)/userland
UOBJDIR  := $(BUILD)/obj/userland

UCFLAGS := -std=gnu11 -O2 -g \
           -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-pie -m64 -march=x86-64 \
           -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
           -Iuserland/include

ULDFLAGS := -nostdlib -static -no-pie -z noexecstack \
            -T userland/link.ld -m elf_x86_64

ULIB_SRC := userland/lib/ulib.c
ULIB_ASM := userland/lib/start.S userland/lib/syscall.S
ULIB_OBJ := $(UDIR)/lib/ulib.o $(UDIR)/lib/start.o $(UDIR)/lib/syscall.o

UELF     := $(patsubst %,$(UDIR)/%.elf,$(UPROGS))
UEMBED   := $(patsubst %,$(UOBJDIR)/%.elf.o,$(UPROGS))

# Wurzelzertifikate und Beispielbilder werden fest eingebaut.
BLOBS    := wurzelzertifikate.der wappen.png muster.png
BLOBOBJ  := $(patsubst %,$(BUILD)/obj/data/%.o,$(BLOBS))

# Der Startsektor-Teil von Limine. Er steckt im Paket nur als C-Header;
# das Installationsprogramm braucht ihn zur Laufzeit als Bytes.
LIMINE_HDD     := $(BUILD)/data/limine-bios-hdd.bin
LIMINE_HDD_OBJ := $(BUILD)/obj/data/limine-bios-hdd.bin.o

CFILES := $(shell find kernel/src -name '*.c' | sort)
SFILES := $(shell find kernel/src -name '*.S' | sort)
OBJS   := $(patsubst kernel/src/%.c,$(BUILD)/obj/%.c.o,$(CFILES)) \
          $(patsubst kernel/src/%.S,$(BUILD)/obj/%.S.o,$(SFILES)) \
          $(UEMBED) $(BLOBOBJ) $(LIMINE_HDD_OBJ)
DEPS   := $(OBJS:.o=.d)

.PHONY: all kernel iso run run-uefi run-plain disk clean distclean limine

# Die uebersetzten Programme nicht nach dem Einbetten wegwerfen.
.SECONDARY: $(UELF) $(ULIB_OBJ)

all: iso

limine:
	@scripts/fetch-limine.sh

kernel: limine $(KERNEL)

# Die Programme werden als Ganzes in den Kernel eingebettet und beim Start
# ins Dateisystem gelegt - so braucht RetroOS keine Festplatte, um sie
# ausfuehren zu koennen.
$(UDIR)/lib/%.o: userland/lib/%.c
	@mkdir -p $(dir $@)
	@echo "  UCC     $<"
	@$(CC) $(UCFLAGS) -c $< -o $@

$(UDIR)/lib/%.o: userland/lib/%.S
	@mkdir -p $(dir $@)
	@echo "  UAS     $<"
	@$(CC) $(UCFLAGS) -c $< -o $@

$(UDIR)/%.elf: userland/%.c $(ULIB_OBJ) userland/link.ld
	@mkdir -p $(dir $@)
	@echo "  UCC     $<"
	@$(CC) $(UCFLAGS) -c $< -o $(UDIR)/$*.o
	@echo "  ULD     $@"
	@$(LD) $(ULDFLAGS) $(UDIR)/$*.o $(ULIB_OBJ) -o $@

$(UOBJDIR)/%.elf.o: $(UDIR)/%.elf
	@mkdir -p $(UOBJDIR)
	@echo "  EMBED   $<"
	@cd $(UDIR) && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    $*.elf $(CURDIR)/$@

$(BUILD)/obj/data/%.o: data/%
	@mkdir -p $(dir $@)
	@echo "  EMBED   $<"
	@cd data && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    $* $(CURDIR)/$@

$(LIMINE_HDD): $(LIMINE)/limine-bios-hdd.h scripts/gen_limine_hdd.py
	@mkdir -p $(dir $@)
	@python3 scripts/gen_limine_hdd.py $< $@

$(LIMINE_HDD_OBJ): $(LIMINE_HDD)
	@mkdir -p $(dir $@)
	@echo "  EMBED   $<"
	@cd $(BUILD)/data && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
	    limine-bios-hdd.bin $(CURDIR)/$@

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

# Ohne Festplatte und ohne Netzwerk - zeigt, dass beides optional ist.
run-plain: iso
	@scripts/run.sh --no-disk --no-net

# Legt eine leere Festplattendatei an. Sie entsteht sonst beim ersten
# "make run" von selbst; hier laesst sich die Groesse vorgeben:
#   make disk DISK_MB=512
DISK    := $(BUILD)/festplatte.img
DISK_MB ?= 256

disk: $(DISK)

$(DISK):
	@mkdir -p $(BUILD)
	@echo "  DISK    $@ ($(DISK_MB) MiB)"
	@qemu-img create -f raw $@ $(DISK_MB)M >/dev/null 2>&1 || \
	 dd if=/dev/zero of=$@ bs=1M count=$(DISK_MB) status=none
	@mkfs.vfat -F 32 -n RETROOS $@ >/dev/null 2>&1 || \
	 echo "  Hinweis: In RetroOS \"formatieren wirklich\" ausfuehren."


# Die Festplattendatei bleibt erhalten - sie enthaelt die Daten des Nutzers.
clean:
	@rm -rf $(BUILD)/obj $(KERNEL) $(ISO_ROOT) $(ISO)

distclean: clean
	@rm -rf third_party
