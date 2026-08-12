#!/bin/bash
set -e

GREEN='\033[0;32m'
NC='\033[0m'


echo -e "${GREEN}=== Building bootloader stages and kernel ===${NC}"

# ---- Компиляция загрузчиков ----
nasm -f bin boot_720.asm -o bin/boot_720.bin
nasm -f bin boot_1440.asm -o bin/boot_1440.bin
nasm -f bin boot_hdd.asm -o bin/boot_hdd.bin
nasm -f bin f12ld.asm -o bin/f12ld.bin
nasm -f bin hddld.asm -o bin/hddld.bin

#xxd -l 16 bin/f12ld.bin

# ---- Дополнение Stage2 до 8 КБ ----
STAGE2_SIZE=8192
echo "STAGE2 SIZE $STAGE2_SIZE"
dd if=bin/f12ld.bin of=bin/f12ld.pad bs=1 count=8192 conv=sync
#xxd -l 16 bin/f12ld.pad
mv bin/f12ld.pad bin/f12ld.bin
#ls -l bin/f12ld.bin

# ---- Ядро ----
if command -v i686-elf-gcc &> /dev/null; then
    CC=i686-elf-gcc
    LD=i686-elf-ld
    OBJCOPY=i686-elf-objcopy
else
    echo "Error: i686-elf-gcc not found."
    exit 1
fi

$CC -m32 -ffreestanding -nostdlib -static -c kernel.c -o bin/kernel.o
$CC -m32 -ffreestanding -nostdlib -static -c draw.c -o bin/draw.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c isr.c -o bin/isr.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c syscalls.c -o bin/syscall.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c font_data.c -o bin/font_data.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c gdt.c -o bin/gdt.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c task.c -o bin/task.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c kernel_heap.c -o bin/kernel_heap.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c pmm.c -o bin/pmm.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c vmm.c -o bin/vmm.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c lib.c -o bin/lib.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c debug.c -o bin/debug.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c bootinfo.c -o bin/bootinfo.o
#$CC -m32 -ffreestanding -nostdlib -Iinclude -c driver.c -o bin/driver.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c driver_elf.c -o bin/driver_elf.o
#$CC -m32 -ffreestanding -nostdlib -Iinclude -c kernel_symbols.c -o bin/kernel_symbols.o
$CC -m32 -ffreestanding -nostdlib -Iinclude -c use_drivers.c -o bin/use_drivers.o

nasm -f elf32 isr.asm -o bin/isr_asm.o
#nasm -f elf32 switch.asm -o bin/switch.o
#nasm -f elf32 idle_loop.asm -o bin/idle_loop.o
#nasm -f elf32 font_data.asm -o bin/font_data.o

#   -----   drivers   -----
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c lib.c  -o bin/dlib.o
# framebuffer driver
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fb_driver/fb_driver.c  -o bin/fb_driver.o
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fb_driver/font_data_driver.c -o bin/font_data_driver.o
nasm -f elf32 drivers/fb_driver/fb_start.asm -o bin/fb_start.o
# fat12 filesystem driver
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fat12_driver/fat12_driver.c  -o bin/fat12_driver.o
#$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fat12_driver/fat12_cluster.c  -o bin/fat12_cluster.o
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fat12_driver/fat12_disk.c  -o bin/fat12_disk.o
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fat12_driver/fat12_boot.c  -o bin/fat12_boot.o
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fat12_driver/fat12_fdc.c  -o bin/fat12_fdc.o
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/fat12_driver/fat12_dma.c  -o bin/fat12_dma.o
nasm -f elf32 drivers/fat12_driver/fat12_start.asm -o bin/fat12_start.o
# ext2 filesystem driver
$CC -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables -c drivers/ext2_driver/ext2_driver.c  -o bin/ext2_driver.o
nasm -f elf32 drivers/ext2_driver/ext2_start.asm -o bin/ext2_start.o


#drivers link
$LD -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld bin/fb_start.o bin/fb_driver.o bin/font_data_driver.o -o bin/fb_driver.elf

$LD -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld \
bin/fat12_start.o bin/fat12_driver.o bin/dlib.o bin/fat12_disk.o \
bin/fat12_boot.o bin/fat12_fdc.o bin/fat12_dma.o -o bin/fat12_driver.elf

$LD -m elf_i386 --emit-relocs -T drivers/userspace_driver.ld bin/ext2_start.o bin/ext2_driver.o -o bin/ext2_driver.elf
#$OBJCOPY -O binary bin/fb_driver.elf bin/fb_driver.bin

#Временный бинарник драйвера в виде массива байт
#xxd -i bin/fb_driver.elf | sed 's/fb_driver_drv/fb_driver/g' > fb_driver_bin2.mk
#echo "" > fb_driver_bin.c
#cat fb_driver_bin.mk fb_driver_bin2.mk > fb_driver_bin.c
#$CC -m32 -ffreestanding -nostdlib -Iinclude -c fb_driver_bin.c -o bin/fb_driver_bin.o


#kernel link
$LD -m elf_i386 -Ttext 0x100000 -o bin/kernel.bin bin/kernel.o bin/lib.o bin/draw.o bin/pmm.o bin/vmm.o bin/isr.o bin/syscall.o bin/isr_asm.o  bin/font_data.o bin/gdt.o bin/task.o bin/debug.o bin/bootinfo.o bin/driver_elf.o bin/use_drivers.o bin/kernel_heap.o --oformat binary
$LD -m elf_i386 -Ttext 0x100000 -o bin/kernel.elf bin/kernel.o bin/lib.o bin/draw.o bin/pmm.o bin/vmm.o bin/isr.o bin/syscall.o bin/isr_asm.o  bin/font_data.o bin/gdt.o bin/task.o bin/debug.o bin/bootinfo.o bin/driver_elf.o bin/use_drivers.o bin/kernel_heap.o


#if $LD -m elf_i386 -Ttext 0x100000 -o bin/kernel.bin bin/kernel.o bin/isr.o bin/syscall.o bin/isr_asm.o bin/draw.o     --oformat binary 2>/dev/null; #then
#    echo "Linked with --oformat binary"
#else
#    echo "Falling back to ELF + objcopy"
#    $LD -m elf_i386 -Ttext 0x100000 -o bin/kernel.elf bin/kernel.o
#    $OBJCOPY -O binary bin/kernel.elf bin/kernel.bin
#    rm -f bin/kernel.elf
#fi

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
#rm -f bin/fat12_driver.elf


# ---- Подготовка содержимого для дискет ----
mkdir -p output tmp_fat12
cp bin/f12ld.bin tmp_fat12/F12.LD
cp bin/kernel.bin tmp_fat12/KERNEL
cp bin/fb_driver.elf tmp_fat12/FB_DR.DRV
cp bin/fat12_driver.elf tmp_fat12/F12_DR.DRV
cp bin/ext2_driver.elf tmp_fat12/EXT2_DR.DRV

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
    mcopy -i "$IMG" tmp_fat12/F12.LD ::/
    mcopy -i "$IMG" tmp_fat12/KERNEL ::/
    mcopy -i "$IMG" tmp_fat12/FB_DR.DRV ::/
    mcopy -i "$IMG" tmp_fat12/F12_DR.DRV ::/
    mcopy -i "$IMG" tmp_fat12/EXT2_DR.DRV ::/
	#echo "xxd tmp_fat12/KERNEL   :"
	#xxd tmp_fat12/KERNEL

    # ---- ПРОСТО ПЕРЕЗАПИСЫВАЕМ ВЕСЬ ЗАГРУЗОЧНЫЙ СЕКТОР ----
    dd if="$BOOT_BIN" of="$IMG" conv=notrunc 2>/dev/null

    #echo "Files in $IMG after boot sector write:"
    #mdir -i "$IMG" ::/ || echo "mdir failed"
}

create_fat_image output/fat12_720.img 720 bin/boot_720.bin
create_fat_image output/fat12_1440.img 1440 bin/boot_1440.bin

rm -rf tmp_fat12

# ---- Подготовка содержимого для HDD (EXT2) ----
mkdir -p tmp_hdd
#cp bin/hddld.bin tmp_hdd/EXT.LD
cp bin/kernel.bin tmp_hdd/kernel
cp bin/fb_driver.elf tmp_hdd/fb_dr.drv
cp bin/fat12_driver.elf tmp_hdd/f12_dr.drv
cp bin/ext2_driver.elf tmp_hdd/ext2_dr.drv


# ---- Создание HDD образа (EXT2 + LBA) ----
echo -e "${GREEN}Creating HDD image (64M) with EXT2 and LBA...${NC}"
HDD_IMG="output/hdd_ext2.img"
dd if=/dev/zero of="$HDD_IMG" bs=1M count=64 2>/dev/null

EXT_SIZE=65536
EXT2_START_LBA=17
#dd if=/dev/zero of=tmp_ext2.img bs=1K count=$EXT_SIZE


if command -v genext2fs &> /dev/null; then
    #genext2fs -b 65536 -d tmp_hdd "$HDD_IMG"
    genext2fs -b $EXT_SIZE -B 1024 -d tmp_hdd tmp_ext2.img
    #dd if=/dev/zero of="$HDD_IMG" bs=1M count=64


#elif command -v mkfs.ext2 &> /dev/null; then
    #echo "genext2fs not found, trying mkfs.ext2 with loop (may need sudo)..."
    #LOOP=$(sudo losetup -f --show "$HDD_IMG")
    #sudo mkfs.ext2 -r 0 -b 1024 "$LOOP" 2>/dev/null
    #sudo mkdir -p /mnt/hdd_ext2
    #sudo mount "$LOOP" /mnt/hdd_ext2
    #sudo cp -r tmp_hdd/* /mnt/hdd_ext2/
    #sudo umount /mnt/hdd_ext2
    #sudo losetup -d "$LOOP"
else
    echo "Error: neither genext2fs. Install genext2fs."
    exit 1
fi

# ---- Запись загрузчиков в HDD ----
#dd if=bin/boot_hdd.bin of="$HDD_IMG" conv=notrunc 2>/dev/null
#dd if=bin/hddld.bin of="$HDD_IMG" bs=512 seek=1 conv=notrunc 2>/dev/null
dd if=bin/boot_hdd.bin of="$HDD_IMG" conv=notrunc
#2>/dev/null
#dd if=bin/hddld.bin of="$HDD_IMG" bs=512 seek=17 conv=notrunc 2>/dev/null
dd if=bin/hddld.bin of="$HDD_IMG" bs=512 seek=1 conv=notrunc

dd if=tmp_ext2.img of="$HDD_IMG" bs=512 seek=$EXT2_START_LBA conv=notrunc

#xxd -l 16 tmp_hdd/kernel

rm -rf tmp_hdd
rm tmp_ext2.img

echo -e "${GREEN}All images created successfully in output/${NC}"
echo -e "${GREEN}Images:${NC}"
#ls -lh output/

#objdump -D bin/fb_driver.elf | head -80
#objdump -R bin/fb_driver.elf

# ---- Запуск QEMU с дискетой или hdd ----
qemu-system-i386 -drive file=output/fat12_720.img,format=raw,if=floppy -vga std -display cocoa -debugcon stdio -global isa-debugcon.iobase=0xe9
#qemu-system-i386 -drive file=output/fat12_1440.img,format=raw,if=floppy -vga std -display cocoa -debugcon stdio -global isa-debugcon.iobase=0xe9
#qemu-system-i386 -drive file=output/hdd_ext2.img,format=raw,if=ide -vga std -display cocoa -debugcon stdio -global isa-debugcon.iobase=0xe9




#file bin/kernel.elf
#target symbols add bin/kernel.elf
#gdb-remote 1234
#breakpoint set --name idle_loop
#/opt/homebrew/opt/binutils/bin/greadelf -S bin/fat12_driver.elf
