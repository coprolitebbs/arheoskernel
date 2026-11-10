# ============================================================================
# Makefile для ArheOS — конвертация из build.sh
#
# Цели:
#   make            — сборка всего + образы (по умолчанию)
#   make kernel     — пересобрать ядро и образы
#   make drivers    — пересобрать драйверы и образы
#   make boot       — пересобрать загрузчики и образы
#   make userspace  — пересобрать user-space программы и образы
#   make images     — только образы
#   make clean      — очистить артефакты
#   make help       — справка
# ============================================================================

# ---- Инструменты ----
CC          := i686-elf-gcc
LD          := i686-elf-ld
OBJCOPY     := i686-elf-objcopy
NASM        := nasm
MKFS_FAT    := mkfs.fat
MCOPY       := mcopy
MMD         := mmd
E2CP        := e2cp
E2MKDIR     := e2mkdir

# Поиск mkfs.ext2 (macOS homebrew + fallback)
MKFS_EXT2 := $(shell \
    if [ -x /opt/homebrew/Cellar/e2fsprogs/1.47.4/sbin/mkfs.ext2 ]; then \
        echo /opt/homebrew/Cellar/e2fsprogs/1.47.4/sbin/mkfs.ext2; \
    elif [ -x /usr/local/sbin/mkfs.ext2 ]; then \
        echo /usr/local/sbin/mkfs.ext2; \
    else \
        command -v mkfs.ext2 2>/dev/null; \
    fi)

# ---- Флаги ----
KERNEL_CCFLAGS   := -m32 -march=i386 -ffreestanding -nostdlib -Iinclude -MMD -MP
DRVFLAGS         := -m32 -march=i386 -ffreestanding -fno-common -fno-stack-protector \
                    -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-pie -MMD -MP
USER_CCFLAGS     := -m32 -march=i386 -ffreestanding -nostdlib -Os -fno-stack-protector \
                    -fno-toplevel-reorder -fno-pie -fno-common -mstringop-strategy=loop \
                    -fno-builtin -DCOMPILE_BIN -DARHEOS_OS -MMD -MP
USER_CCFLAGS_ELF := -m32 -march=i386 -ffreestanding -fPIE -fno-stack-protector \
                    -fno-builtin -Os -DARHEOS_OS -MMD -MP

USER_LDFLAGS_ELF := -m elf_i386 -pie -e _start
KERNEL_LDFLAGS   := -m elf_i386
DRIVER_LDFLAGS   := -m elf_i386 --no-warn-rwx-segments --emit-relocs

BIN    := bin
OUT    := output
TMPEXT := tmp_ext2.img
SPROCS := userspace/system\ procs

# ---- Списки файлов ----
BOOT_BINS := $(BIN)/boot_720.bin $(BIN)/boot_1440.bin $(BIN)/boot_hdd.bin \
             $(BIN)/f12ld.bin $(BIN)/hddld.bin

KERNEL_OBJS := \
    $(BIN)/kernel.o $(BIN)/draw.o $(BIN)/isr.o $(BIN)/syscall.o \
    $(BIN)/font_data.o $(BIN)/gdt.o $(BIN)/task.o $(BIN)/kernel_heap.o \
    $(BIN)/pmm.o $(BIN)/vmm.o $(BIN)/lib.o $(BIN)/debug.o $(BIN)/bootinfo.o \
    $(BIN)/driver_elf.o $(BIN)/use_drivers.o $(BIN)/ports_io.o \
    $(BIN)/comdebug.o $(BIN)/hardware.o $(BIN)/vfs.o $(BIN)/kconsole.o \
    $(BIN)/config.o $(BIN)/kbd_translate.o $(BIN)/font.o $(BIN)/locale.o \
    $(BIN)/font_vga.o $(BIN)/kpanic.o $(BIN)/kload_screen.o \
    $(BIN)/usb_registry.o $(BIN)/graphic_layer.o $(BIN)/sound_layer.o \
    $(BIN)/vbe_rm.o $(BIN)/isr_asm.o $(BIN)/vbe_rm_blob.o

KERNEL_BINS := $(BIN)/kernel.bin $(BIN)/kernel.elf

# Драйверы (ELF)
FB_OBJS      := $(BIN)/fb_start.o $(BIN)/fb_driver.o $(BIN)/dlib.o $(BIN)/d_font.o
VESA_OBJS    := $(BIN)/vesa_start.o $(BIN)/vesa_fb.o $(BIN)/dlib.o
FAT12_OBJS   := $(BIN)/fat12_start.o $(BIN)/fat12_driver.o $(BIN)/dlib.o \
                $(BIN)/fat12_disk.o $(BIN)/fat12_boot.o $(BIN)/fat12_fdc.o \
                $(BIN)/fat12_dma.o $(BIN)/drv_ports_io.o
EXT2_OBJS    := $(BIN)/ext2_start.o $(BIN)/ext2_driver.o $(BIN)/ext2_disk.o \
                $(BIN)/drv_ports_io.o $(BIN)/dlib.o
KBD_OBJS     := $(BIN)/kbd_start.o $(BIN)/kbd_driver.o $(BIN)/drv_ports_io.o $(BIN)/dlib.o
BLOCK_OBJS   := $(BIN)/block_start.o $(BIN)/block_driver.o $(BIN)/drv_ports_io.o $(BIN)/dlib.o
USB_OBJS     := $(BIN)/usb_start.o $(BIN)/usb_core.o $(BIN)/usb_uhci.o \
                $(BIN)/usb_msc.o $(BIN)/usb_ehci.o $(BIN)/usb_driver.o \
                $(BIN)/drv_ports_io.o $(BIN)/dlib.o

DRIVER_ELFS := $(BIN)/fb_driver.elf $(BIN)/vesa_fb.elf $(BIN)/fat12_driver.elf \
               $(BIN)/ext2_driver.elf $(BIN)/kbd_driver.elf $(BIN)/block_driver.elf \
               $(BIN)/usb_driver.elf

# User-space flat-bin
FLAT_BINS := $(BIN)/hello.bin $(BIN)/vfs_test.bin $(BIN)/ucat.bin \
             $(BIN)/kb_check.bin $(BIN)/allocram.bin \
             $(BIN)/chorus.bin $(BIN)/halt.bin $(BIN)/cat.bin $(BIN)/ls.bin \
             $(BIN)/free.bin $(BIN)/touch.bin $(BIN)/rm.bin $(BIN)/mkdir.bin \
             $(BIN)/rmdir.bin $(BIN)/setbg.bin

# User-space ELF
USER_ELFS := $(BIN)/arr_test.elf $(BIN)/sc_test.elf $(BIN)/sekhmet.elf \
             $(BIN)/thoth.elf $(BIN)/lsusb.elf $(BIN)/arfetch.elf \
             $(BIN)/grinfo.elf $(BIN)/df.elf $(BIN)/arcom.elf

USER_ARTIFACTS := $(FLAT_BINS) $(USER_ELFS)

IMAGES := $(OUT)/fat12_720.img $(OUT)/fat12_1440.img $(OUT)/hdd_ext2.img \
          $(OUT)/usbimage.img $(OUT)/fat12_1440_usb.img

# Файлы, копируемые в FAT-образы
FAT_USER := $(BIN)/sekhmet.elf $(BIN)/halt.bin $(BIN)/chorus.bin $(BIN)/cat.bin \
            $(BIN)/ls.bin $(BIN)/free.bin $(BIN)/touch.bin $(BIN)/rm.bin \
            $(BIN)/mkdir.bin $(BIN)/rmdir.bin $(BIN)/setbg.bin $(BIN)/thoth.elf \
            $(BIN)/lsusb.elf $(BIN)/arfetch.elf $(BIN)/df.elf $(BIN)/arcom.elf \
            $(BIN)/grinfo.elf

FAT_CFG := userspace/filesystem/config/boot/boot.cfg \
           userspace/filesystem/config/sekhmet/config.cfg \
           userspace/filesystem/config/sekhmet/sekhmet.hlp \
           userspace/filesystem/config/locales/en.cfg \
           userspace/filesystem/config/locales/ru.cfg

FAT_DEPS := $(BIN)/kernel.bin $(BIN)/f12ld.bin \
            $(BIN)/fb_driver.elf $(BIN)/block_driver.elf $(BIN)/usb_driver.elf \
            $(BIN)/fat12_driver.elf $(BIN)/ext2_driver.elf \
            $(BIN)/kbd_driver.elf $(BIN)/vesa_fb.elf \
            $(FAT_USER) $(FAT_CFG)

# Файлы, копируемые в HDD/ext2 образ
HDD_USER := $(FAT_USER) $(BIN)/hello.bin
HDD_USBIN := $(BIN)/kb_check.bin $(BIN)/arr_test.elf $(BIN)/sc_test.elf
HDD_CFG  := $(FAT_CFG) \
            userspace/filesystem/config/img/sekhmet.png \
            userspace/filesystem/config/img/ewall.png

HDD_DEPS := $(BIN)/kernel.bin $(BIN)/boot_hdd.bin $(BIN)/hddld.bin \
            $(BIN)/fb_driver.elf $(BIN)/block_driver.elf $(BIN)/usb_driver.elf \
            $(BIN)/fat12_driver.elf $(BIN)/ext2_driver.elf \
            $(BIN)/kbd_driver.elf $(BIN)/vesa_fb.elf \
            $(HDD_USER) $(HDD_USBIN) $(HDD_CFG)

# ============================================================================
# Phony-цели
# ============================================================================
.PHONY: all kernel drivers boot userspace images clean help print-%

all: $(BOOT_BINS) $(KERNEL_BINS) $(DRIVER_ELFS) $(USER_ARTIFACTS) images

kernel:    $(KERNEL_BINS) images
drivers:   $(DRIVER_ELFS) images
boot:      $(BOOT_BINS) images
userspace: $(USER_ARTIFACTS) images
images:    $(IMAGES)

# ============================================================================
# Директории
# ============================================================================
$(BIN) $(OUT):
	mkdir -p $@

# ============================================================================
# Загрузчики
# ============================================================================
$(BIN)/boot_720.bin:  boot_720.asm  | $(BIN)
	$(NASM) -f bin $< -o $@
$(BIN)/boot_1440.bin: boot_1440.asm | $(BIN)
	$(NASM) -f bin $< -o $@
$(BIN)/boot_hdd.bin:  boot_hdd.asm  | $(BIN)
	$(NASM) -f bin $< -o $@
$(BIN)/f12ld.bin:     f12ld.asm     | $(BIN)
	$(NASM) -f bin $< -o $@
$(BIN)/hddld.bin:     hddld.asm     | $(BIN)
	$(NASM) -f bin $< -o $@

# ============================================================================
# Ядро — объектные файлы
# ============================================================================
# Основное правило: имя .o совпадает с именем .c
$(BIN)/%.o: %.c | $(BIN)
	$(CC) $(KERNEL_CCFLAGS) -c $< -o $@

# Исключение: syscalls.c -> syscall.o
$(BIN)/syscall.o: syscalls.c | $(BIN)
	$(CC) $(KERNEL_CCFLAGS) -c $< -o $@

# ASM-файлы ядра
$(BIN)/isr_asm.o: isr.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@
$(BIN)/vbe_rm_blob.o: vbe_rm_blob.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# ============================================================================
# Линковка ядра
# ============================================================================
$(BIN)/kernel.bin: $(KERNEL_OBJS) kernel.ld
	$(LD) $(KERNEL_LDFLAGS) -T kernel.ld -o $@ $(KERNEL_OBJS)

$(BIN)/kernel.elf: $(KERNEL_OBJS) kernel_elf.ld
	$(LD) $(KERNEL_LDFLAGS) -T kernel_elf.ld -o $@ $(KERNEL_OBJS)

# ============================================================================
# Драйверы — объектные файлы
# ============================================================================
# Общая библиотека и шрифт для драйверов
$(BIN)/dlib.o: lib.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/d_font.o: font.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/drv_ports_io.o: ports_io.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@

# Framebuffer
$(BIN)/fb_driver.o: drivers/fb_driver/fb_driver.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/fb_start.o: drivers/fb_driver/fb_start.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# FAT12
$(BIN)/fat12_driver.o: drivers/fat12_driver/fat12_driver.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/fat12_disk.o: drivers/fat12_driver/fat12_disk.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/fat12_boot.o: drivers/fat12_driver/fat12_boot.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/fat12_fdc.o: drivers/fat12_driver/fat12_fdc.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/fat12_dma.o: drivers/fat12_driver/fat12_dma.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/fat12_start.o: drivers/fat12_driver/fat12_start.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# EXT2
$(BIN)/ext2_driver.o: drivers/ext2_driver/ext2_driver.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/ext2_disk.o: drivers/ext2_driver/ext2_disk.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/ext2_start.o: drivers/ext2_driver/ext2_start.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# PS/2 клавиатура
$(BIN)/kbd_driver.o: drivers/kbd_driver/kbd_driver.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/kbd_start.o: drivers/kbd_driver/kbd_start.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# VESA framebuffer
$(BIN)/vesa_fb.o: drivers/vesa_fb/vesa_fb.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/vesa_start.o: drivers/vesa_fb/vesa_start.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# Block device
$(BIN)/block_driver.o: drivers/block_driver/block_driver.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/block_start.o: drivers/block_driver/block_start.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# USB
$(BIN)/usb_core.o:   drivers/usb_driver/usb_core.c   | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/usb_uhci.o:   drivers/usb_driver/usb_uhci.c   | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/usb_msc.o:    drivers/usb_driver/usb_msc.c    | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/usb_ehci.o:   drivers/usb_driver/usb_ehci.c   | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/usb_driver.o: drivers/usb_driver/usb_driver.c | $(BIN)
	$(CC) $(DRVFLAGS) -c $< -o $@
$(BIN)/usb_start.o:  drivers/usb_driver/usb_start.asm | $(BIN)
	$(NASM) -f elf32 $< -o $@

# ============================================================================
# Линковка драйверов
# ============================================================================
$(BIN)/fb_driver.elf: $(FB_OBJS) drivers/userspace_driver.ld
	$(LD) $(DRIVER_LDFLAGS) -T drivers/userspace_driver.ld $(FB_OBJS) -o $@

$(BIN)/vesa_fb.elf: $(VESA_OBJS) drivers/userspace_driver.ld
	$(LD) $(DRIVER_LDFLAGS) -T drivers/userspace_driver.ld $(VESA_OBJS) -o $@

$(BIN)/fat12_driver.elf: $(FAT12_OBJS) drivers/userspace_driver.ld
	$(LD) $(DRIVER_LDFLAGS) -T drivers/userspace_driver.ld $(FAT12_OBJS) -o $@

$(BIN)/ext2_driver.elf: $(EXT2_OBJS) drivers/userspace_driver.ld
	$(LD) $(DRIVER_LDFLAGS) -T drivers/userspace_driver.ld $(EXT2_OBJS) -o $@

$(BIN)/kbd_driver.elf: $(KBD_OBJS) drivers/userspace_driver.ld
	$(LD) $(DRIVER_LDFLAGS) -T drivers/userspace_driver.ld $(KBD_OBJS) -o $@

$(BIN)/block_driver.elf: $(BLOCK_OBJS) drivers/userspace_driver.ld
	$(LD) $(DRIVER_LDFLAGS) -T drivers/userspace_driver.ld $(BLOCK_OBJS) -o $@

$(BIN)/usb_driver.elf: $(USB_OBJS) drivers/userspace_driver.ld
	$(LD) $(DRIVER_LDFLAGS) -T drivers/userspace_driver.ld $(USB_OBJS) -o $@

# ============================================================================
# User-space — объектные файлы (библиотеки)
# ============================================================================
$(BIN)/u_std.o: userspace/lib/ustd.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@
$(BIN)/u_std_elf.o: userspace/lib/ustd.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

$(BIN)/u_memory.o: userspace/lib/umemory.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@
$(BIN)/u_memory_elf.o: userspace/lib/umemory.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

$(BIN)/u_lib.o: lib.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@
$(BIN)/u_lib_elf.o: lib.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

$(BIN)/u_uconfig.o: userspace/lib/uconfig.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@
$(BIN)/u_uconfig_elf.o: userspace/lib/uconfig.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

$(BIN)/u_uimage.o: userspace/lib/uimage.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@
$(BIN)/u_uimage_elf.o: userspace/lib/uimage.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

$(BIN)/u_udevices.o: userspace/lib/udevices.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@
$(BIN)/u_udevices_elf.o: userspace/lib/udevices.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

$(BIN)/u_ugraph.o: userspace/lib/ugraph.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@
$(BIN)/u_ugraph_elf.o: userspace/lib/ugraph.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

# ============================================================================
# User-space — объектные файлы (примеры)
# ============================================================================
$(BIN)/u_hello.o: userspace/examples/hello/hello.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BIN)/u_vfs_test.o: userspace/examples/vfs_test/vfs_test.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BIN)/u_ucat.o: userspace/examples/cat/ucat.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BIN)/u_kb_check.o: userspace/examples/kb_check/kb_check.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BIN)/u_allocram.o: userspace/examples/allocram/allocram.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c $< -o $@

$(BIN)/u_arr_test.o: userspace/examples/arr_test/arr_test.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

$(BIN)/u_scanf_check.o: userspace/examples/scanf_check/scanf_check.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c $< -o $@

# ============================================================================
# User-space — объектные файлы (системные процессы)
# ============================================================================
$(BIN)/u_sekhmet.o: $(SPROCS)/sekhmet/sekhmet.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_chorus.o: $(SPROCS)/chorus/chorus.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_halt.o: $(SPROCS)/halt/halt.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_cat.o: $(SPROCS)/cat/cat.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_ls.o: $(SPROCS)/ls/ls.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_free.o: $(SPROCS)/free/free.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_touch.o: $(SPROCS)/touch/touch.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_rm.o: $(SPROCS)/rm/rm.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_mkdir.o: $(SPROCS)/mkdir/mkdir.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_rmdir.o: $(SPROCS)/rmdir/rmdir.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_setbg.o: $(SPROCS)/setbg/setbg.c | $(BIN)
	$(CC) $(USER_CCFLAGS) -c "$<" -o "$@"

$(BIN)/u_thoth.o: $(SPROCS)/thoth/thoth.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_thoth_platform.o: $(SPROCS)/thoth/platform_arheos.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_lsusb.o: $(SPROCS)/lsusb/lsusb.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_arfetch.o: $(SPROCS)/arfetch/arfetch.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_graphinfo.o: $(SPROCS)/graphinfo/graphinfo.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_df.o: $(SPROCS)/df/df.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_arcom_ui.o: $(SPROCS)/arcom/fm_ui.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_arcom_core.o: $(SPROCS)/arcom/fm_core.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_arcom_ah.o: $(SPROCS)/arcom/arheos/fm_arheos.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

$(BIN)/u_arcom_am.o: $(SPROCS)/arcom/arheos/main_arheos.c | $(BIN)
	$(CC) $(USER_CCFLAGS_ELF) -c "$<" -o "$@"

# ============================================================================
# Линковка user-space — flat-бинарники
# ============================================================================
FLATBIN_LD := $(LD) -m elf_i386 --oformat binary -N \
              -T userspace/lib/userspace.ld -Ttext 0x40000000 -e _start

$(BIN)/hello.bin: $(BIN)/u_std.o $(BIN)/u_hello.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/vfs_test.bin: $(BIN)/u_std.o $(BIN)/u_vfs_test.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/ucat.bin: $(BIN)/u_std.o $(BIN)/u_ucat.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/kb_check.bin: $(BIN)/u_std.o $(BIN)/u_memory.o $(BIN)/u_kb_check.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/allocram.bin: $(BIN)/u_std.o $(BIN)/u_memory.o $(BIN)/u_allocram.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/chorus.bin: $(BIN)/u_std.o $(BIN)/u_chorus.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/halt.bin: $(BIN)/u_std.o $(BIN)/u_halt.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/cat.bin: $(BIN)/u_std.o $(BIN)/u_cat.o $(BIN)/u_lib.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/ls.bin: $(BIN)/u_std.o $(BIN)/u_ls.o $(BIN)/u_lib.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/free.bin: $(BIN)/u_std.o $(BIN)/u_memory.o $(BIN)/u_free.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/touch.bin: $(BIN)/u_std.o $(BIN)/u_touch.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/rm.bin: $(BIN)/u_std.o $(BIN)/u_rm.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/mkdir.bin: $(BIN)/u_std.o $(BIN)/u_mkdir.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/rmdir.bin: $(BIN)/u_std.o $(BIN)/u_rmdir.o
	$(FLATBIN_LD) $^ -o $@
$(BIN)/setbg.bin: $(BIN)/u_std.o $(BIN)/u_lib.o $(BIN)/u_memory.o \
                  $(BIN)/u_uimage.o $(BIN)/u_setbg.o
	$(FLATBIN_LD) $^ -o $@

# ============================================================================
# Линковка user-space — ELF-файлы
# ============================================================================
$(BIN)/arr_test.elf: $(BIN)/u_std_elf.o $(BIN)/u_memory_elf.o $(BIN)/u_arr_test.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/sc_test.elf: $(BIN)/u_std_elf.o $(BIN)/u_scanf_check.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/sekhmet.elf: $(BIN)/u_std_elf.o $(BIN)/u_memory_elf.o $(BIN)/u_lib_elf.o \
                    $(BIN)/u_uconfig_elf.o $(BIN)/u_uimage_elf.o $(BIN)/u_sekhmet.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/thoth.elf: $(BIN)/u_std_elf.o $(BIN)/u_memory_elf.o \
                   $(BIN)/u_thoth.o $(BIN)/u_thoth_platform.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/lsusb.elf: $(BIN)/u_std_elf.o $(BIN)/u_udevices_elf.o $(BIN)/u_lsusb.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/arfetch.elf: $(BIN)/u_std_elf.o $(BIN)/u_memory_elf.o $(BIN)/u_lib_elf.o \
                    $(BIN)/u_udevices_elf.o $(BIN)/u_uimage_elf.o $(BIN)/u_arfetch.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/grinfo.elf: $(BIN)/u_std_elf.o $(BIN)/u_ugraph_elf.o $(BIN)/u_graphinfo.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/df.elf: $(BIN)/u_std_elf.o $(BIN)/u_lib_elf.o $(BIN)/u_df.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@
$(BIN)/arcom.elf: $(BIN)/u_std_elf.o $(BIN)/u_memory_elf.o $(BIN)/u_lib_elf.o \
                  $(BIN)/u_arcom_ui.o $(BIN)/u_arcom_core.o \
                  $(BIN)/u_arcom_ah.o $(BIN)/u_arcom_am.o
	$(LD) $(USER_LDFLAGS_ELF) $^ -o $@

# ============================================================================
# Образы
# ============================================================================
# Макрос создания FAT12-дискеты
#   $1 = target image, $2 = size KB, $3 = sectors/cluster, $4 = boot binary
define create_fat_image
	set -e; \
	dd if=/dev/zero of="$1" bs=1024 count=$2 2>/dev/null; \
	$(MKFS_FAT) -F 12 -f 2 -s $3 -r 224 -R 1 -v "$1" 2>/dev/null; \
	mcopy -i "$1" $(BIN)/f12ld.bin ::/F12.LD; \
	mcopy -i "$1" $(BIN)/kernel.bin ::/KERNEL; \
	mcopy -i "$1" $(BIN)/fb_driver.elf ::/FB_DR.DRV; \
	mcopy -i "$1" $(BIN)/block_driver.elf ::/BLOCK.DRV; \
	mcopy -i "$1" $(BIN)/usb_driver.elf ::/USB.DRV; \
	mcopy -i "$1" $(BIN)/fat12_driver.elf ::/F12_DR.DRV; \
	mcopy -i "$1" $(BIN)/ext2_driver.elf ::/EXT2_DR.DRV; \
	mmd -i "$1" ::/ADRV; \
	mcopy -i "$1" $(BIN)/kbd_driver.elf ::/ADRV/KBDPS2.DRV; \
	mcopy -i "$1" $(BIN)/vesa_fb.elf ::/ADRV/VESA.DRV; \
	mmd -i "$1" ::/USBIN; \
	mmd -i "$1" ::/BIN; \
	mcopy -i "$1" $(BIN)/sekhmet.elf ::/BIN/; \
	mcopy -i "$1" $(BIN)/halt.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/chorus.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/cat.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/ls.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/free.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/touch.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/rm.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/mkdir.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/rmdir.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/setbg.bin ::/BIN/; \
	mcopy -i "$1" $(BIN)/thoth.elf ::/BIN/; \
	mcopy -i "$1" $(BIN)/lsusb.elf ::/BIN/; \
	mcopy -i "$1" $(BIN)/arfetch.elf ::/BIN/; \
	mcopy -i "$1" $(BIN)/df.elf ::/BIN/; \
	mcopy -i "$1" $(BIN)/arcom.elf ::/BIN/; \
	mcopy -i "$1" $(BIN)/grinfo.elf ::/BIN/; \
	mmd -i "$1" ::/CONFIG; \
	mmd -i "$1" ::/CONFIG/BOOT; \
	mmd -i "$1" ::/CONFIG/SEKHMET; \
	mmd -i "$1" ::/CONFIG/IMG; \
	mmd -i "$1" ::/CONFIG/LOCALES; \
	mcopy -i "$1" userspace/filesystem/config/boot/boot.cfg ::/CONFIG/BOOT/BOOT.CFG; \
	mcopy -i "$1" userspace/filesystem/config/sekhmet/config.cfg ::/CONFIG/SEKHMET/CONFIG.CFG; \
	mcopy -i "$1" userspace/filesystem/config/sekhmet/sekhmet.hlp ::/CONFIG/SEKHMET/SEKHMET.HLP; \
	mcopy -i "$1" userspace/filesystem/config/locales/en.cfg ::/CONFIG/LOCALES/EN.CFG; \
	mcopy -i "$1" userspace/filesystem/config/locales/ru.cfg ::/CONFIG/LOCALES/RU.CFG; \
	dd if="$4" of="$1" conv=notrunc 2>/dev/null
endef

$(OUT)/fat12_720.img: $(FAT_DEPS) $(BIN)/boot_720.bin | $(OUT)
	@echo "=== Creating FAT12 image: $@ ==="
	$(call create_fat_image,$@,720,2,$(BIN)/boot_720.bin)

$(OUT)/fat12_1440.img: $(FAT_DEPS) $(BIN)/boot_1440.bin | $(OUT)
	@echo "=== Creating FAT12 image: $@ ==="
	$(call create_fat_image,$@,1440,1,$(BIN)/boot_1440.bin)

# HDD ext2 image
$(OUT)/hdd_ext2.img: $(HDD_DEPS) | $(OUT)
	@if [ -z "$(MKFS_EXT2)" ]; then echo "Error: mkfs.ext2 not found. Install e2fsprogs."; exit 1; fi
	@echo "=== Creating HDD ext2 image: $@ ==="
	set -e; \
	rm -f "$@"; \
	dd if=/dev/zero of="$@" bs=1M count=64 2>/dev/null; \
	rm -f $(TMPEXT); \
	dd if=/dev/zero of=$(TMPEXT) bs=512 count=131008 2>/dev/null; \
	$(MKFS_EXT2) -T small -b 1024 -I 128 -O ^filetype,^dir_index,^ext_attr -F $(TMPEXT); \
	$(E2CP) $(BIN)/kernel.bin        $(TMPEXT):/kernel; \
	$(E2CP) $(BIN)/fb_driver.elf     $(TMPEXT):/fb_dr.drv; \
	$(E2CP) $(BIN)/block_driver.elf  $(TMPEXT):/block.drv; \
	$(E2CP) $(BIN)/usb_driver.elf    $(TMPEXT):/usb.drv; \
	$(E2CP) $(BIN)/fat12_driver.elf  $(TMPEXT):/f12_dr.drv; \
	$(E2CP) $(BIN)/ext2_driver.elf   $(TMPEXT):/ext2_dr.drv; \
	$(E2MKDIR) $(TMPEXT):/adrv; \
	$(E2CP) $(BIN)/kbd_driver.elf    $(TMPEXT):/adrv/kbdps2.drv; \
	$(E2CP) $(BIN)/usb_driver.elf    $(TMPEXT):/adrv/usb.drv; \
	$(E2CP) $(BIN)/vesa_fb.elf       $(TMPEXT):/adrv/vesa.drv; \
	$(E2MKDIR) $(TMPEXT):/usbin; \
	$(E2MKDIR) $(TMPEXT):/bin; \
	$(E2CP) $(BIN)/kb_check.bin      $(TMPEXT):/usbin/kb_check.bin; \
	$(E2CP) $(BIN)/arr_test.elf      $(TMPEXT):/usbin/arr_test.elf; \
	$(E2CP) $(BIN)/sc_test.elf       $(TMPEXT):/usbin/sc_test.elf; \
	$(E2CP) $(BIN)/sekhmet.elf       $(TMPEXT):/bin/sekhmet.elf; \
	$(E2CP) $(BIN)/halt.bin          $(TMPEXT):/bin/halt.bin; \
	$(E2CP) $(BIN)/hello.bin         $(TMPEXT):/bin/hello.bin; \
	$(E2CP) $(BIN)/chorus.bin        $(TMPEXT):/bin/chorus.bin; \
	$(E2CP) $(BIN)/cat.bin           $(TMPEXT):/bin/cat.bin; \
	$(E2CP) $(BIN)/ls.bin            $(TMPEXT):/bin/ls.bin; \
	$(E2CP) $(BIN)/free.bin          $(TMPEXT):/bin/free.bin; \
	$(E2CP) $(BIN)/touch.bin         $(TMPEXT):/bin/touch.bin; \
	$(E2CP) $(BIN)/rm.bin            $(TMPEXT):/bin/rm.bin; \
	$(E2CP) $(BIN)/mkdir.bin         $(TMPEXT):/bin/mkdir.bin; \
	$(E2CP) $(BIN)/rmdir.bin         $(TMPEXT):/bin/rmdir.bin; \
	$(E2CP) $(BIN)/setbg.bin         $(TMPEXT):/bin/setbg.bin; \
	$(E2CP) $(BIN)/thoth.elf         $(TMPEXT):/bin/thoth.elf; \
	$(E2CP) $(BIN)/lsusb.elf         $(TMPEXT):/bin/lsusb.elf; \
	$(E2CP) $(BIN)/arfetch.elf       $(TMPEXT):/bin/arfetch.elf; \
	$(E2CP) $(BIN)/df.elf            $(TMPEXT):/bin/df.elf; \
	$(E2CP) $(BIN)/arcom.elf         $(TMPEXT):/bin/arcom.elf; \
	$(E2CP) $(BIN)/grinfo.elf     $(TMPEXT):/bin/grinfo.elf; \
	$(E2MKDIR) $(TMPEXT):/config; \
	$(E2MKDIR) $(TMPEXT):/config/boot; \
	$(E2MKDIR) $(TMPEXT):/config/sekhmet; \
	$(E2MKDIR) $(TMPEXT):/config/img; \
	$(E2MKDIR) $(TMPEXT):/config/locales; \
	$(E2CP) userspace/filesystem/config/boot/boot.cfg               $(TMPEXT):/config/boot/boot.cfg; \
	$(E2CP) userspace/filesystem/config/sekhmet/config.cfg          $(TMPEXT):/config/sekhmet/config.cfg; \
	$(E2CP) userspace/filesystem/config/sekhmet/sekhmet.hlp         $(TMPEXT):/config/sekhmet/sekhmet.hlp; \
	$(E2CP) userspace/filesystem/config/img/sekhmet.png             $(TMPEXT):/config/img/sekhmet.png; \
	$(E2CP) userspace/filesystem/config/img/ewall.png               $(TMPEXT):/config/img/ewall.png; \
	$(E2CP) userspace/filesystem/config/locales/en.cfg              $(TMPEXT):/config/locales/en.cfg; \
	$(E2CP) userspace/filesystem/config/locales/ru.cfg              $(TMPEXT):/config/locales/ru.cfg; \
	dd if=$(BIN)/boot_hdd.bin of="$@" conv=notrunc 2>/dev/null; \
	dd if=$(BIN)/hddld.bin of="$@" bs=512 seek=1 conv=notrunc 2>/dev/null; \
	dd if=$(TMPEXT) of="$@" bs=512 seek=64 conv=notrunc 2>/dev/null; \
	rm -f $(TMPEXT)

# USB-образ (копия HDD)
$(OUT)/usbimage.img: $(OUT)/hdd_ext2.img
	cp $< $@

# USB-дискета (копия fat12_1440 + замена конфига)
$(OUT)/fat12_1440_usb.img: $(OUT)/fat12_1440.img \
                           userspace/filesystem/config/boot/boot_usbtest.cfg
	cp $< $@
	mcopy -o -i $@ userspace/filesystem/config/boot/boot_usbtest.cfg \
	    ::/CONFIG/BOOT/BOOT.CFG

# ============================================================================
# Очистка
# ============================================================================
clean:
	rm -rf $(BIN) $(OUT)
	rm -f $(TMPEXT)

# ============================================================================
# Справка и утилиты
# ============================================================================
help:
	@echo "Доступные цели:"
	@echo "  all (по умолчанию) — собрать всё + образы"
	@echo "  kernel             — пересобрать ядро и образы"
	@echo "  drivers            — пересобрать драйверы и образы"
	@echo "  boot               — пересобрать загрузчики и образы"
	@echo "  userspace          — пересобрать userspace и образы"
	@echo "  images             — только образы"
	@echo "  clean              — удалить артефакты сборки"

print-%:
	@echo '$* = $($*)'

# ============================================================================
# Автозависимости (заголовки)
# ============================================================================
-include $(wildcard $(BIN)/*.d)

# ============================================================================
# Поведение при ошибках
# ============================================================================
.DELETE_ON_ERROR:

