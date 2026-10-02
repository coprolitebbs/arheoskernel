#include "include/fat12_disk.h"
#include "include/fat12_internal.h"
#include "include/fat12_boot.h"
//#include "include/fat12_io.h"
#include "include/fat12_fdc.h"
#include "include/fat12_dma.h"
#include "include/fat12_driver.h"
#include "../include_drivers/drv_format.h"
#include "../../include-kernel/ports_io.h"


//флаг IRQ6
static volatile int floppy_irq_done = 0;

//конверт lba в chs

static void lba_to_chs(fat12_fs_t *vol,uint32_t lba,uint8_t *cylinder,uint8_t *head,uint8_t *sector){
    uint32_t spt = vol->sectors_per_track;
    uint32_t heads = vol->heads;

    *cylinder = lba / (heads * spt);
    uint32_t tmp = lba % (heads * spt);
    *head = tmp / spt;
    *sector = (tmp % spt) + 1;
}


//Чтение одного сектора
static int floppy_read_sector(fat12_fs_t *vol,uint32_t lba,void *buffer){
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;
    lba_to_chs(vol,lba,&cylinder,&head,&sector);
    //Настраиваем DMA на 512 байт.
    dma_floppy_read(buffer,512);
    //Выбираем drive 0 и включаем motor.
    outb(FDC_DOR,FDC_DOR_ENABLE | FDC_DOR_DMA_IRQ | FDC_DOR_MOTOR0 | FDC_DOR_DRIVE0);
    io_wait();

    //READ DATA
    //MT  MFM  SK
    // 0   1   0
    int r;

    r = fdc_send_byte(FDC_CMD_READ_DATA | 0x40);
    if (r != FS_OK) return r;

    //Drive/head
    r = fdc_send_byte((head << 2) | 0);
    if (r != FS_OK) return r;

    //Cylinder
    r = fdc_send_byte(cylinder);
    if (r != FS_OK) return r;

    //Head
    r = fdc_send_byte(head);
    if (r != FS_OK) return r;

    //Sector
    r = fdc_send_byte(sector);
    if (r != FS_OK) return r;

    //Sector size:
    //2 = 512 bytes
    r = fdc_send_byte(2);
    if (r != FS_OK) return r;

    // EOT
    //Читаем только один сектор.
    r = fdc_send_byte((uint8_t)vol->sectors_per_track);
    if (r != FS_OK) return r;

    //GAP3
    r = fdc_send_byte(0x1B);
    if (r != FS_OK) return r;

    //Data length.
    r = fdc_send_byte(0xFF);
    if (r != FS_OK) return r;

    //В полноценной реализации здесь ждём IRQ6.
    //Для первого теста просто ждём, пока контроллер закончит busy.
    uint32_t timeout = 2000000;
    while (timeout--){
        uint8_t msr = inb(FDC_MSR);
        if (!(msr & FDC_MSR_CB)) break;
    }
    if (!timeout) return FS_IO_ERROR;
    //Получаем 7 байт результата.
    uint8_t result[7];
    for (int i = 0; i < 7; i++){
        r = fdc_receive_byte(&result[i]);
        if (r != FS_OK) return r;
    }

    //ST0
    if (result[0] & 0xC0) return FS_IO_ERROR;
    //ST1/ST2 errors.
    if (result[1] & 0xBF) return FS_IO_ERROR;
    if (result[2] & 0x7F) return FS_IO_ERROR;

    return FS_OK;
}


//Чтение сектора
 int fat12_read_sectors(fat12_fs_t *vol,uint32_t lba,uint32_t count,void *buffer){
    if (!vol || !buffer) return FS_IO_ERROR;
    if (count == 0) return FS_OK;

    //Пока поддерживаем только стандартные 512-байтовые сектора, но, больше для дискет и не надо
    if (vol->bytes_per_sector != 512) return FS_NOT_SUPPORTED;
    uint8_t *dst = (uint8_t *)buffer;
    for (uint32_t i = 0; i < count; i++){
        //Читаем один физический сектор через FDC
        uint32_t current_lba = lba + i;
        uint32_t sectors_per_cylinder = vol->heads * vol->sectors_per_track;
        uint32_t cylinder = current_lba / sectors_per_cylinder;
        uint32_t tmp = current_lba % sectors_per_cylinder;
        uint32_t head = tmp / vol->sectors_per_track;
        uint32_t sector = (tmp % vol->sectors_per_track) + 1;
        int r = fdc_read_sector(vol,cylinder,head,sector,dst + i * vol->bytes_per_sector);
        if (r != FS_OK) return r;
    }

    return FS_OK;
}


//Запись сектора
int fat12_write_sectors(fat12_fs_t *vol,uint32_t lba,uint32_t count,const void *buffer){
    if (!vol || !buffer || count == 0) return FS_IO_ERROR;
    if (vol->bytes_per_sector != 512) return FS_NOT_SUPPORTED;

    const uint8_t *src = (const uint8_t *)buffer;

    for (uint32_t i = 0; i < count; i++){
        uint32_t current_lba = lba + i;
        uint32_t sectors_per_cylinder = vol->heads * vol->sectors_per_track;
        uint32_t cylinder = current_lba / sectors_per_cylinder;
        uint32_t tmp = current_lba % sectors_per_cylinder;
        uint32_t head = tmp / vol->sectors_per_track;
        uint32_t sector = (tmp % vol->sectors_per_track) + 1;

        int r = fdc_write_sector(vol, cylinder, head, sector, src + i * 512);
        if (r != FS_OK) return r;
    }

    return FS_OK;
}



//Инициализация
int fat12_disk_init(fat12_fs_t *vol){
    if (!vol) return FS_IO_ERROR;
    //Пока поддерживаем только floppy A:
    if (vol->drive != 0) return FS_NOT_SUPPORTED;
    //Стандартная геометрия будет уточнена после чтения BPB.
    //Временно ставим 18 секторов/трек.

    vol->sectors_per_track = 18;
    vol->heads = 2;
    //Сброс FDC
    int r = fdc_reset();
    if (r != FS_OK) return r;
    //Возвращаем головку на cylinder 0.
    r = fdc_recalibrate();
    if (r != FS_OK) return r;

    return FS_OK;
}


uint16_t fat12_get_next_cluster(fat12_fs_t *vol,uint16_t cluster){
    uint32_t offset;

    if (!vol) return 0xFFF;

    offset = cluster + (cluster / 2);

    if (offset + 1 >= vol->sectors_per_fat * vol->bytes_per_sector){
        return 0xFFF;
    }
    uint16_t value = vol->fat[offset] | ((uint16_t)vol->fat[offset + 1] << 8);
    if (cluster & 1)
        value >>= 4;
    else
        value &= 0x0FFF;

    return value & 0x0FFF;
}
