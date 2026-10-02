#!/bin/bash
set -e

GREEN='\033[0;32m'
NC='\033[0m'


echo -e "${GREEN}=== Building bootloader stages and kernel ===${NC}"

# ---- Компиляция загрузчиков ----
nasm -f bin boot_720.asm -o bin/boot_720.bin
nasm -f bin boot_1440.asm -o bin/boot_1440.bin
nasm -f bin boot_hdd.asm -o bin/boot_hdd.bin
echo "123" > bin/f12ld.bin
#nasm -f bin f12ld.asm -o bin/f12ld.bin
nasm -f bin hddld.asm -o bin/hddld.bin

#xxd -l 16 bin/f12ld.bin

# ---- Дополнение Stage2 до 8 КБ ----
STAGE2_SIZE=8192
echo "STAGE2 SIZE $STAGE2_SIZE"
dd if=bin/f12ld.bin of=bin/f12ld.pad bs=1 count=8192 conv=sync
#xxd -l 16 bin/f12ld.pad
mv bin/f12ld.pad bin/f12ld.bin
#ls -l bin/f12ld.bin

dd if=bin/hddld.bin of=bin/hddld.pad bs=1 count=8192 conv=sync
mv bin/hddld.pad bin/hddld.bin

# ---- Ядро ----
if command -v i686-elf-gcc &> /dev/null; then
    CC=i686-elf-gcc
    LD=i686-elf-ld
    OBJCOPY=i686-elf-objcopy

    # -march=i386 принудительно запрещает компилятору использовать инструкции Pentium Pro (включая cmov)
    KERNEL_CCFLAGS="-m32 -march=i386 -ffreestanding -nostdlib"
    DRVFLAGS="-m32 -march=i386 -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables"
    #USER_CCFLAGS="-m32 -march=i386 -ffreestanding -nostdlib -fno-stack-protector -fno-pie -mstringop-strategy=loop -fno-builtin"
    USER_CCFLAGS="-m32 -march=i386 -ffreestanding -nostdlib -Os -fno-stack-protector -fno-toplevel-reorder -fno-pie -fno-common -mstringop-strategy=loop -fno-builtin"
    USER_CCFLAGS_ELF="-m32 -march=i386 -ffreestanding -fPIE -fno-stack-protector -fno-builtin"
    USER_LDFLAGS_ELF="-m elf_i386 -pie -e _start"

else
    echo "Error: i686-elf-gcc not found."
    exit 1
fi

$CC $KERNEL_CCFLAGS -static -c kernel.c -o bin/kernel.o
$CC $KERNEL_CCFLAGS -static -c draw.c -o bin/draw.o
$CC $KERNEL_CCFLAGS -Iinclude -c isr.c -o bin/isr.o
$CC $KERNEL_CCFLAGS -Iinclude -c syscalls.c -o bin/syscall.o
$CC $KERNEL_CCFLAGS -Iinclude -c font_data.c -o bin/font_data.o
$CC $KERNEL_CCFLAGS -Iinclude -c gdt.c -o bin/gdt.o
$CC $KERNEL_CCFLAGS -Iinclude -c task.c -o bin/task.o
$CC $KERNEL_CCFLAGS -Iinclude -c kernel_heap.c -o bin/kernel_heap.o
$CC $KERNEL_CCFLAGS -Iinclude -c pmm.c -o bin/pmm.o
$CC $KERNEL_CCFLAGS -Iinclude -c vmm.c -o bin/vmm.o
$CC $KERNEL_CCFLAGS -Iinclude -c lib.c -o bin/lib.o
$CC $KERNEL_CCFLAGS -Iinclude -c debug.c -o bin/debug.o
$CC $KERNEL_CCFLAGS -Iinclude -c bootinfo.c -o bin/bootinfo.o
#$CC $KERNEL_CCFLAGS -Iinclude -c driver.c -o bin/driver.o
$CC $KERNEL_CCFLAGS -Iinclude -c driver_elf.c -o bin/driver_elf.o
#$CC $KERNEL_CCFLAGS -Iinclude -c kernel_symbols.c -o bin/kernel_symbols.o
$CC $KERNEL_CCFLAGS -Iinclude -c use_drivers.c -o bin/use_drivers.o
$CC $KERNEL_CCFLAGS -Iinclude -c ports_io.c -o bin/ports_io.o
$CC $KERNEL_CCFLAGS -Iinclude -c comdebug.c -o bin/comdebug.o
$CC $KERNEL_CCFLAGS -Iinclude -c hardware.c -o bin/hardware.o
$CC $KERNEL_CCFLAGS -Iinclude -c vfs.c -o bin/vfs.o
$CC $KERNEL_CCFLAGS -Iinclude -c kconsole.c -o bin/kconsole.o
$CC $KERNEL_CCFLAGS -Iinclude -c config.c -o bin/config.o

nasm -f elf32 isr.asm -o bin/isr_asm.o
#nasm -f elf32 switch.asm -o bin/switch.o
#nasm -f elf32 idle_loop.asm -o bin/idle_loop.o
#nasm -f elf32 font_data.asm -o bin/font_data.o

#   -----   drivers   -----
$CC $DRVFLAGS -fno-pie -c lib.c  -o bin/dlib.o
# framebuffer driver
$CC $DRVFLAGS -fno-pie -c drivers/fb_driver/fb_driver.c  -o bin/fb_driver.o
$CC $DRVFLAGS -fno-pie -c drivers/fb_driver/font_data_driver.c -o bin/font_data_driver.o
nasm -f elf32 drivers/fb_driver/fb_start.asm -o bin/fb_start.o

# fat12 filesystem driver
$CC $DRVFLAGS -fno-pie -c drivers/fat12_driver/fat12_driver.c  -o bin/fat12_driver.o
#$CC $DRVFLAGS -fno-pie -c drivers/fat12_driver/fat12_cluster.c  -o bin/fat12_cluster.o
$CC $DRVFLAGS -fno-pie -c drivers/fat12_driver/fat12_disk.c  -o bin/fat12_disk.o
$CC $DRVFLAGS -fno-pie -c drivers/fat12_driver/fat12_boot.c  -o bin/fat12_boot.o
$CC $DRVFLAGS -fno-pie -c drivers/fat12_driver/fat12_fdc.c  -o bin/fat12_fdc.o
$CC $DRVFLAGS -fno-pie -c drivers/fat12_driver/fat12_dma.c  -o bin/fat12_dma.o
$CC $DRVFLAGS -fno-pie -c ports_io.c  -o bin/drv_ports_io.o
nasm -f elf32 drivers/fat12_driver/fat12_start.asm -o bin/fat12_start.o

# ext2 filesystem driver
$CC $DRVFLAGS -fno-pie -c drivers/ext2_driver/ext2_driver.c  -o bin/ext2_driver.o
$CC $DRVFLAGS -fno-pie -c drivers/ext2_driver/ext2_disk.c  -o bin/ext2_disk.o
nasm -f elf32 drivers/ext2_driver/ext2_start.asm -o bin/ext2_start.o

# keyboard ps/2 driver
$CC $DRVFLAGS -fno-pie -c drivers/kbd_driver/kbd_driver.c  -o bin/kbd_driver.o
nasm -f elf32 drivers/kbd_driver/kbd_start.asm -o bin/kbd_start.o

#drivers link
$LD -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld bin/fb_start.o \
bin/fb_driver.o bin/font_data_driver.o bin/dlib.o -o bin/fb_driver.elf

$LD -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld \
bin/fat12_start.o bin/fat12_driver.o bin/dlib.o bin/fat12_disk.o \
bin/fat12_boot.o bin/fat12_fdc.o bin/fat12_dma.o bin/drv_ports_io.o -o bin/fat12_driver.elf

$LD -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld bin/ext2_start.o bin/ext2_driver.o \
bin/ext2_disk.o bin/drv_ports_io.o bin/dlib.o -o bin/ext2_driver.elf
#$OBJCOPY -O binary bin/fb_driver.elf bin/fb_driver.bin

$LD -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld bin/kbd_start.o bin/kbd_driver.o \
bin/drv_ports_io.o bin/dlib.o -o bin/kbd_driver.elf

#Временный бинарник драйвера в виде массива байт
#xxd -i bin/fb_driver.elf | sed 's/fb_driver_drv/fb_driver/g' > fb_driver_bin2.mk
#echo "" > fb_driver_bin.c
#cat fb_driver_bin.mk fb_driver_bin2.mk > fb_driver_bin.c
#$CC -m32 -ffreestanding -nostdlib -Iinclude -c fb_driver_bin.c -o bin/fb_driver_bin.o


#kernel link
$LD -m elf_i386 -Ttext 0x100000 -o bin/kernel.bin bin/kernel.o bin/lib.o bin/draw.o \
bin/pmm.o bin/vmm.o bin/isr.o bin/syscall.o bin/isr_asm.o  bin/font_data.o bin/gdt.o \
bin/task.o bin/debug.o bin/bootinfo.o bin/driver_elf.o bin/use_drivers.o bin/kernel_heap.o \
bin/comdebug.o bin/ports_io.o bin/hardware.o bin/vfs.o bin/kconsole.o bin/config.o \
--oformat binary

$LD -m elf_i386 -Ttext 0x100000 -o bin/kernel.elf bin/kernel.o bin/lib.o bin/draw.o \
bin/pmm.o bin/vmm.o bin/isr.o bin/syscall.o bin/isr_asm.o  bin/font_data.o bin/gdt.o \
bin/task.o bin/debug.o bin/bootinfo.o bin/driver_elf.o bin/use_drivers.o bin/kernel_heap.o \
bin/comdebug.o bin/ports_io.o bin/hardware.o bin/vfs.o bin/kconsole.o bin/config.o


# 🚀 СБОРКА ПРОСТРАНСТВА ПОЛЬЗОВАТЕЛЯ (USERSPACE)
echo -e "${GREEN}=== Building userspace libraries and applications ===${NC}"
# Рантайм-заглушка точки входа (entry.asm)
#nasm -f elf32 userspace/lib/entry.asm -o bin/u_entry.o
# Си-библиотека пользовательских оберток (ustd.c)
$CC $USER_CCFLAGS -c userspace/lib/ustd.c -o bin/u_std.o
#Библиотека работы с памятью для пользовательских приложений
$CC $USER_CCFLAGS -c userspace/lib/umemory.c -o bin/u_memory.o
#Библиотечка стандартных функций
$CC $USER_CCFLAGS -c lib.c -o bin/u_lib.o
#Библиотечка чтения секционированных конфигов
$CC $USER_CCFLAGS -c userspace/lib/uconfig.c -o bin/u_uconfig.o
#Библиотечка работы с изображениями
$CC $USER_CCFLAGS -c userspace/lib/uimage.c -o bin/u_uimage.o

# hello.c
$CC $USER_CCFLAGS -c userspace/examples/hello/hello.c -o bin/u_hello.o
#vfs_test.c
$CC $USER_CCFLAGS -c userspace/examples/vfs_test/vfs_test.c -o bin/u_vfs_test.o
#cat test
$CC $USER_CCFLAGS -c userspace/examples/cat/ucat.c -o bin/u_ucat.o
#kbd test
$CC $USER_CCFLAGS -c userspace/examples/kb_check/kb_check.c -o bin/u_kb_check.o
#memory test
$CC $USER_CCFLAGS -c userspace/examples/allocram/allocram.c -o bin/u_allocram.o
#array test
$CC $USER_CCFLAGS_ELF -c userspace/examples/arr_test/arr_test.c -o bin/u_arr_test.o

#sekhmet
$CC $USER_CCFLAGS -c "userspace/system procs/sekhmet/sekhmet.c" -o bin/u_sekhmet.o
#horus
$CC $USER_CCFLAGS -c "userspace/system procs/horus/horus.c" -o bin/u_horus.o
#halt
$CC $USER_CCFLAGS -c "userspace/system procs/halt/halt.c" -o bin/u_halt.o
#cat
$CC $USER_CCFLAGS -c "userspace/system procs/cat/cat.c" -o bin/u_cat.o
#ls
$CC $USER_CCFLAGS -c "userspace/system procs/ls/ls.c" -o bin/u_ls.o
#free
$CC $USER_CCFLAGS -c "userspace/system procs/free/free.c" -o bin/u_free.o
#touch
$CC $USER_CCFLAGS -c "userspace/system procs/touch/touch.c" -o bin/u_touch.o
#rm
$CC $USER_CCFLAGS -c "userspace/system procs/rm/rm.c" -o bin/u_rm.o
#mkdir
$CC $USER_CCFLAGS -c "userspace/system procs/mkdir/mkdir.c" -o bin/u_mkdir.o
#rmdir
$CC $USER_CCFLAGS -c "userspace/system procs/rmdir/rmdir.c" -o bin/u_rmdir.o
#setbg
$CC $USER_CCFLAGS -c "userspace/system procs/setbg/setbg.c" -o bin/u_setbg.o

# Линк flat-бинарника без ELF-заголовков
# Базовый адрес -Ttext 0x40000000 совпадает с картой памяти процессов.
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_hello.o -o bin/hello.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_vfs_test.o -o bin/vfs_test.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_ucat.o -o bin/ucat.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_kb_check.o -o bin/kb_check.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_memory.o bin/u_allocram.o -o bin/allocram.bin
#$LD -m elf_i386 -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_memory.o bin/u_arr_test.o -o bin/arr_test.elf
$LD $USER_LDFLAGS bin/u_std.o bin/u_memory.o bin/u_arr_test.o -o bin/arr_test.elf

$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 \
-e _start bin/u_std.o bin/u_memory.o bin/u_lib.o bin/u_uconfig.o bin/u_sekhmet.o \
bin/u_uimage.o -o bin/sekhmet.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_horus.o -o bin/horus.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_halt.o -o bin/halt.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_cat.o bin/u_lib.o -o bin/cat.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_ls.o bin/u_lib.o -o bin/ls.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_memory.o bin/u_free.o -o bin/free.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_touch.o -o bin/touch.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_rm.o -o bin/rm.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_mkdir.o -o bin/mkdir.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_rmdir.o -o bin/rmdir.bin
$LD -m elf_i386 --oformat binary -N -T ./userspace/lib/userspace.ld -Ttext 0x40000000 -e _start bin/u_std.o bin/u_lib.o bin/u_memory.o bin/u_uimage.o bin/u_setbg.o -o bin/setbg.bin


#clear kernel
rm -f bin/kernel.o
rm -f bin/isr.o
rm -f bin/draw.o
rm -f bin/syscall.o
rm -f bin/isr_asm.o
rm -f bin/font_data.o
rm -f bin/gdt.o
rm -f bin/task.o
rm -f bin/kernel_heap.o
rm -f bin/pmm.o
#rm -f bin/switch.o
rm -f bin/vmm.o
rm -f bin/debug.o
#rm -f bin/idle_loop.o
rm -f bin/bootinfo.o
rm -f bin/driver.o
rm -f bin/driver_elf.o
#rm -f bin/fb_driver_bin.o
#rm -f bin/kernel_symbols.o
rm -f bin/use_drivers.o
rm -f bin/ports_io.o
rm -f bin/comdebug.o
rm -f bin/hardware.o
rm -f bin/vfs.o
rm -f bin/kconsole.o

#clear drivers
rm -f bin/dlib.o
rm -f bin/fb_driver.o
#rm -f bin/font_driver.o
rm -f bin/font_data_driver.o
rm -f bin/fb_start.o
#rm -f bin/fb_driver.elf
rm -f bin/fat12_start.o
#rm -f bin/fat12_cluster.o
rm -f bin/fat12_disk.o
rm -f bin/fat12_boot.o
rm -f bin/fat12_fdc.o
rm -f bin/fat12_dma.o
rm -f bin/ext2_driver.o
rm -f bin/ext2_start.o
rm -f bin/drv_ports_io.o
#rm -f bin/fat12_driver.elf
rm -f bin/kbd_driver.o
rm -f bin/kbd_start.o

#clear userspace bin
rm -f bin/u_*.o

# ---- Подготовка содержимого для дискет ----
#mkdir -p output tmp_fat12
#cp bin/f12ld.bin tmp_fat12/F12.LD
#cp bin/kernel.bin tmp_fat12/KERNEL
#cp bin/fb_driver.elf tmp_fat12/FB_DR.DRV
#cp bin/fat12_driver.elf tmp_fat12/F12_DR.DRV
#cp bin/ext2_driver.elf tmp_fat12/EXT2_DR.DRV
#mkdir tmp_fat12/adrv
#cp bin/kbd_driver.elf tmp_fat12/adrv/KBDPS2.DRV

#cp bin/hello.bin tmp_fat12/HELLO.BIN
#cp bin/vfs_test.bin tmp_fat12/VFS_TEST.BIN
#cp bin/cat.bin tmp_fat12/CAT.BIN
#cp bin/kb_check.bin tmp_fat12/KB_CHECK.BIN
#cp bin/allocram.bin tmp_fat12/ALLOCRAM.BIN

#cp bin/sekhmet.bin tmp_fat12/SEKHMET.BIN
#cp bin/horus.bin tmp_fat12/HORUS.BIN
#cp bin/halt.bin tmp_fat12/HALT.BIN

#echo "1234567890" > tmp_fat12/test.txt

# ---- FAT-образы (дискеты) ----
create_fat_image() {
    local IMG=$1
    local SIZE=$2
    local BOOT_BIN=$3

    echo -e "${GREEN}Creating FAT12 image: $IMG (${SIZE}K) with $BOOT_BIN${NC}"

    dd if=/dev/zero of="$IMG" bs=1024 count="$SIZE" 2>/dev/null
	if [ "$SIZE" -eq 1440 ]; then
        mkfs.fat -F 12 -f 2 -s 1 -r 224 -R 1 -v "$IMG" 2>/dev/null
    else
		mkfs.fat -F 12 -f 2 -s 2 -r 224 -R 1 -v "$IMG" 2>/dev/null
	fi
    # Копируем файлы в образ
    #Загрузчик stage2
    mcopy -i "$IMG" bin/f12ld.bin ::/F12.LD
    #само ядро
    mcopy -i "$IMG" bin/kernel.bin ::/KERNEL
    #основные драйвера фреймбуффера и файловых систем FAT12 и EXT2
    mcopy -i "$IMG" bin/fb_driver.elf ::/FB_DR.DRV
    mcopy -i "$IMG" bin/fat12_driver.elf ::/F12_DR.DRV
    mcopy -i "$IMG" bin/ext2_driver.elf ::/EXT2_DR.DRV
    mmd -i "$IMG" ::/ADRV
    mcopy -i "$IMG" bin/kbd_driver.elf ::/ADRV/KBDPS2.DRV
    #пользовательские программы
    mmd -i "$IMG" ::/USBIN
    mcopy -i "$IMG" bin/hello.bin ::/USBIN/
    mcopy -i "$IMG" bin/vfs_test.bin ::/USBIN/
    mcopy -i "$IMG" bin/ucat.bin ::/USBIN/
    mcopy -i "$IMG" bin/kb_check.bin ::/USBIN/
    mcopy -i "$IMG" bin/allocram.bin ::/USBIN/
    mmd -i "$IMG" ::/BIN
    mcopy -i "$IMG" bin/sekhmet.bin ::/BIN/
    mcopy -i "$IMG" bin/halt.bin ::/BIN/
    mcopy -i "$IMG" bin/hello.bin ::/BIN/
    mcopy -i "$IMG" bin/horus.bin ::/BIN/
    mcopy -i "$IMG" bin/cat.bin ::/BIN/
    mcopy -i "$IMG" bin/ls.bin ::/BIN/
    mcopy -i "$IMG" bin/free.bin ::/BIN/
    mcopy -i "$IMG" bin/touch.bin ::/BIN/
    mcopy -i "$IMG" bin/rm.bin ::/BIN/
    mcopy -i "$IMG" bin/mkdir.bin ::/BIN/
    mcopy -i "$IMG" bin/rmdir.bin ::/BIN/
    mcopy -i "$IMG" bin/setbg.bin ::/BIN/

    mmd -i "$IMG" ::/CONFIG
    mmd -i "$IMG" ::/CONFIG/BOOT
    mmd -i "$IMG" ::/CONFIG/SEKHMET
    mmd -i "$IMG" ::/CONFIG/IMG

    mcopy -i "$IMG" userspace/filesystem/config/boot/boot.cfg ::/CONFIG/BOOT/BOOT.CFG
    mcopy -i "$IMG" userspace/filesystem/config/sekhmet/config.cfg ::/CONFIG/SEKHMET/CONFIG.CFG
    mcopy -i "$IMG" userspace/filesystem/config/sekhmet/sekhmet.hlp ::/CONFIG/SEKHMET/SEKHMET.HLP
    #mcopy -i "$IMG" userspace/filesystem/config/img/sekhmet.tga ::/CONFIG/IMG/SEKHMET.TGA


    # ---- ПРОСТО ПЕРЕЗАПИСЫВАЕМ ВЕСЬ ЗАГРУЗОЧНЫЙ СЕКТОР ----
    dd if="$BOOT_BIN" of="$IMG" conv=notrunc 2>/dev/null

    #echo "Files in $IMG after boot sector write:"
    #mdir -i "$IMG" ::/ || echo "mdir failed"
}

create_fat_image output/fat12_720.img 720 bin/boot_720.bin
create_fat_image output/fat12_1440.img 1440 bin/boot_1440.bin

#rm -rf tmp_fat12

# ---- Подготовка содержимого для HDD (EXT2) ----
#rm -rf tmp_hdd
#mkdir -p tmp_hdd
#cp bin/hddld.bin tmp_hdd/EXT.LD
#cp bin/kernel.bin tmp_hdd/kernel
#cp bin/fb_driver.elf tmp_hdd/fb_dr.drv
#cp bin/fat12_driver.elf tmp_hdd/f12_dr.drv
#cp bin/ext2_driver.elf tmp_hdd/ext2_dr.drv
#mkdir tmp_hdd/adrv
#cp bin/kbd_driver.elf tmp_hdd/adrv/kbdps2.drv
#cp bin/hello.bin tmp_hdd/hello.bin
#cp bin/vfs_test.bin tmp_hdd/vfs_test.bin
#cp bin/cat.bin tmp_hdd/cat.bin
#cp bin/kb_check.bin tmp_hdd/kb_check.bin
#cp bin/allocram.bin tmp_hdd/allocram.bin
#cp bin/sekhmet.bin tmp_hdd/sekhmet.bin
#cp bin/halt.bin tmp_hdd/halt.bin
#cp bin/horus.bin tmp_hdd/horus.bin

#echo "1234567890" > tmp_hdd/test.txt

sync
sleep 1

# ---- Создание HDD образа (EXT2 + LBA) ----
echo -e "${GREEN}Creating HDD image (64M) with EXT2 and LBA...${NC}"
HDD_IMG="output/hdd_ext2.img"
#dd if=/dev/zero of="$HDD_IMG" bs=1M count=64 2>/dev/null


if [ -f "/opt/homebrew/Cellar/e2fsprogs/1.47.4/sbin/mkfs.ext2" ]; then
    MKFS_EXT2="/opt/homebrew/Cellar/e2fsprogs/1.47.4/sbin/mkfs.ext2"
elif [ -f "/usr/local/sbin/mkfs.ext2" ]; then
    MKFS_EXT2="/usr/local/sbin/mkfs.ext2"
else
    echo "Ошибка: e2fsprogs не найден. Установите: brew install e2fsprogs"
    exit 1
fi

if ! command -v e2cp &> /dev/null; then
    echo "Ошибка: e2tools не найден. Установите: brew install e2tools"
    exit 1
fi

rm -f "$HDD_IMG"
rm -f tmp_ext2.img
dd if=/dev/zero of=tmp_ext2.img bs=512 count=131008 2>/dev/null
$MKFS_EXT2 -T small -b 1024 -I 128 -O ^filetype,^dir_index,^ext_attr -F tmp_ext2.img

#EXT_SIZE=65536
EXT2_START_LBA=64
#dd if=/dev/zero of=tmp_ext2.img bs=1K count=$EXT_SIZE


#if command -v genext2fs &> /dev/null; then
    #genext2fs -b $EXT_SIZE -B 1024 -N 500 -d tmp_hdd tmp_ext2.img
    #genext2fs -b $EXT_SIZE -B 1024 -N 64 -d tmp_hdd tmp_ext2.img

#else
    #echo "Error: neither genext2fs. Install genext2fs."
    #exit 1
#fi



e2cp bin/kernel.bin tmp_ext2.img:/kernel
e2cp bin/fb_driver.elf tmp_ext2.img:/fb_dr.drv
e2cp bin/fat12_driver.elf tmp_ext2.img:/f12_dr.drv
e2cp bin/ext2_driver.elf tmp_ext2.img:/ext2_dr.drv
e2mkdir tmp_ext2.img:/adrv
e2cp bin/kbd_driver.elf tmp_ext2.img:/adrv/kbdps2.drv

e2mkdir tmp_ext2.img:/usbin
e2mkdir tmp_ext2.img:/bin

#e2cp bin/hello.bin tmp_ext2.img:/usbin/hello.bin
#e2cp bin/kb_check.bin tmp_ext2.img:/usbin/kb_check.bin
#e2cp bin/vfs_test.bin tmp_ext2.img:/usbin/vfs_test.bin
#e2cp bin/allocram.bin tmp_ext2.img:/usbin/allocram.bin
#e2cp bin/ucat.bin tmp_ext2.img:/usbin/ucat.bin
e2cp bin/arr_test.elf tmp_ext2.img:/usbin/arr_test.elf

e2cp bin/sekhmet.bin tmp_ext2.img:/bin/sekhmet.bin
e2cp bin/halt.bin tmp_ext2.img:/bin/halt.bin
e2cp bin/hello.bin tmp_ext2.img:/bin/hello.bin
e2cp bin/horus.bin tmp_ext2.img:/bin/horus.bin
e2cp bin/cat.bin tmp_ext2.img:/bin/cat.bin
e2cp bin/ls.bin tmp_ext2.img:/bin/ls.bin
e2cp bin/free.bin tmp_ext2.img:/bin/free.bin
e2cp bin/touch.bin tmp_ext2.img:/bin/touch.bin
e2cp bin/rm.bin tmp_ext2.img:/bin/rm.bin
e2cp bin/mkdir.bin tmp_ext2.img:/bin/mkdir.bin
e2cp bin/rmdir.bin tmp_ext2.img:/bin/rmdir.bin
e2cp bin/setbg.bin tmp_ext2.img:/bin/setbg.bin

e2mkdir tmp_ext2.img:/config
e2mkdir tmp_ext2.img:/config/boot
e2mkdir tmp_ext2.img:/config/sekhmet
e2mkdir tmp_ext2.img:/config/img

e2cp userspace/filesystem/config/boot/boot.cfg tmp_ext2.img:/config/boot/boot.cfg
e2cp userspace/filesystem/config/sekhmet/config.cfg tmp_ext2.img:/config/sekhmet/config.cfg
e2cp userspace/filesystem/config/sekhmet/sekhmet.hlp tmp_ext2.img:/config/sekhmet/sekhmet.hlp
e2cp userspace/filesystem/config/img/sekhmet.tga tmp_ext2.img:/config/img/sekhmet.tga


# ---- Запись загрузчиков в HDD ----
#dd if=bin/boot_hdd.bin of="$HDD_IMG" conv=notrunc 2>/dev/null
#dd if=bin/hddld.bin of="$HDD_IMG" bs=512 seek=1 conv=notrunc 2>/dev/null
dd if=bin/boot_hdd.bin of="$HDD_IMG" conv=notrunc
#2>/dev/null
#dd if=bin/hddld.bin of="$HDD_IMG" bs=512 seek=17 conv=notrunc 2>/dev/null
dd if=bin/hddld.bin of="$HDD_IMG" bs=512 seek=1 conv=notrunc
dd if=tmp_ext2.img of="$HDD_IMG" bs=512 seek=$EXT2_START_LBA conv=notrunc

#dd if=tmp_ext2.img of="$HDD_IMG" bs=512 seek=$EXT2_START_LBA conv=notrunc
#dd if=bin/kernel.bin of="$HDD_IMG" bs=512 seek=2048 conv=notrunc

#xxd -l 16 tmp_hdd/kernel

#rm -rf tmp_hdd
rm tmp_ext2.img



echo -e "${GREEN}All images created successfully in output/${NC}"
echo -e "${GREEN}Images:${NC}"
#ls -lh output/

#objdump -D bin/fb_driver.elf | head -80
#objdump -R bin/fb_driver.elf

# ---- Запуск QEMU с дискетой или hdd ----
#qemu-system-i386 -drive file=output/fat12_720.img,format=raw,if=floppy -vga std -display cocoa -debugcon stdio -global isa-debugcon.iobase=0xe9
#qemu-system-i386 -drive file=output/fat12_1440.img,format=raw,if=floppy -vga std -display cocoa -debugcon stdio -global isa-debugcon.iobase=0xe9
qemu-system-i386 -drive file=output/hdd_ext2.img,format=raw,if=ide -vga std -display cocoa -debugcon stdio -global isa-debugcon.iobase=0xe9




#file bin/kernel.elf
#target symbols add bin/kernel.elf
#gdb-remote 1234
#breakpoint set --name idle_loop
#/opt/homebrew/opt/binutils/bin/greadelf -S bin/fat12_driver.elf
