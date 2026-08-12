#include "include/fat12_disk.h"
#include "include/fat12_internal.h"
#include "include/fat12_boot.h"
#include "include/fat12_io.h"
#include "include/fat12_fdc.h"
#include "../include_drivers/drv_format.h"
/* ============================================================
 * Floppy Disk Controller
 * ============================================================ */

#define FDC_DOR        0x3F2
#define FDC_MSR        0x3F4
#define FDC_FIFO       0x3F5
#define FDC_CCR        0x3F7


/* Digital Output Register */

#define FDC_DOR_DRIVE0     0x00
#define FDC_DOR_DRIVE1     0x01

#define FDC_DOR_RESET      0x00
#define FDC_DOR_ENABLE     0x04
#define FDC_DOR_DMA_IRQ    0x08

#define FDC_DOR_MOTOR0     0x10
#define FDC_DOR_MOTOR1     0x20


/* Main Status Register */

#define FDC_MSR_RQM        0x80
#define FDC_MSR_DIO        0x40
#define FDC_MSR_NDMA       0x20
#define FDC_MSR_CB         0x10


/* Commands */

#define FDC_CMD_SPECIFY        0x03
#define FDC_CMD_RECALIBRATE    0x07
#define FDC_CMD_SENSE_INTERRUPT 0x08
#define FDC_CMD_READ_DATA      0x46
#define FDC_CMD_WRITE_DATA     0x05
#define FDC_CMD_SEEK           0x0F


/* ============================================================
 * DMA channel 2
 * ============================================================ */

#define DMA_MASK_REG           0x0A
#define DMA_MODE_REG           0x0B
#define DMA_CLEAR_FF_REG       0x0C

#define DMA_CH2_ADDR           0x04
#define DMA_CH2_COUNT          0x05
#define DMA_PAGE_CH2           0x81




/* ============================================================
 * IRQ6
 * ============================================================ */

static volatile int floppy_irq_done = 0;


/* ============================================================
 * DMA setup
 * ============================================================ */

static void dma_floppy_read(
    void *buffer,
    uint16_t count
)
{
    uint32_t addr = (uint32_t)buffer;

    /*
     * Запрещаем канал 2.
     */

    outb(
        DMA_MASK_REG,
        0x06
    );

    /*
     * Сбрасываем flip-flop.
     */

    outb(
        DMA_CLEAR_FF_REG,
        0
    );

    /*
     * Адрес.
     */

    outb(
        DMA_CH2_ADDR,
        (uint8_t)(addr & 0xFF)
    );

    outb(
        DMA_CH2_ADDR,
        (uint8_t)((addr >> 8) & 0xFF)
    );

    /*
     * Page register.
     */

    outb(
        DMA_PAGE_CH2,
        (uint8_t)((addr >> 16) & 0xFF)
    );

    /*
     * Снова сбрасываем flip-flop.
     */

    outb(
        DMA_CLEAR_FF_REG,
        0
    );

    /*
     * count = количество байт - 1
     */

    count--;

    outb(
        DMA_CH2_COUNT,
        (uint8_t)(count & 0xFF)
    );

    outb(
        DMA_CH2_COUNT,
        (uint8_t)((count >> 8) & 0xFF)
    );

    /*
     * Режим:
     *
     * channel 2
     * single
     * address increment
     * read from device → memory
     */

    outb(
        DMA_MODE_REG,
        0x46
    );

    /*
     * Разрешаем канал 2.
     */

    outb(
        DMA_MASK_REG,
        0x02
    );
}


/* ============================================================
 * CHS
 * ============================================================ */

static void lba_to_chs(
    fat12_fs_t *vol,
    uint32_t lba,
    uint8_t *cylinder,
    uint8_t *head,
    uint8_t *sector
)
{
    uint32_t spt = vol->sectors_per_track;
    uint32_t heads = vol->heads;

    *cylinder =
        lba / (heads * spt);

    uint32_t tmp =
        lba % (heads * spt);

    *head =
        tmp / spt;

    *sector =
        (tmp % spt) + 1;
}


/* ============================================================
 * Read one sector
 * ============================================================ */

static int floppy_read_sector(
    fat12_fs_t *vol,
    uint32_t lba,
    void *buffer
)
{
    uint8_t cylinder;
    uint8_t head;
    uint8_t sector;

    lba_to_chs(
        vol,
        lba,
        &cylinder,
        &head,
        &sector
    );

    /*
     * Настраиваем DMA на 512 байт.
     */

    dma_floppy_read(
        buffer,
        512
    );

    /*
     * Выбираем drive 0 и включаем motor.
     */

    outb(
        FDC_DOR,
        FDC_DOR_ENABLE |
        FDC_DOR_DMA_IRQ |
        FDC_DOR_MOTOR0 |
        FDC_DOR_DRIVE0
    );

    io_wait();

    /*
     * READ DATA
     *
     * MT  MFM  SK
     *  0   1   0
     */

    int r;

    r = fdc_send_byte(
        FDC_CMD_READ_DATA | 0x40
    );

    if (r != FS_OK)
        return r;

    /*
     * Drive/head
     */

    r = fdc_send_byte(
        (head << 2) | 0
    );

    if (r != FS_OK)
        return r;

    /*
     * Cylinder
     */

    r = fdc_send_byte(cylinder);

    if (r != FS_OK)
        return r;

    /*
     * Head
     */

    r = fdc_send_byte(head);

    if (r != FS_OK)
        return r;

    /*
     * Sector
     */

    r = fdc_send_byte(sector);

    if (r != FS_OK)
        return r;

    /*
     * Sector size:
     *
     * 2 = 512 bytes
     */

    r = fdc_send_byte(2);

    if (r != FS_OK)
        return r;

    /*
     * EOT.
     *
     * Читаем только один сектор.
     */

    r = fdc_send_byte(
        (uint8_t)vol->sectors_per_track
    );

    if (r != FS_OK)
        return r;

    /*
     * GAP3.
     */

    r = fdc_send_byte(0x1B);

    if (r != FS_OK)
        return r;

    /*
     * Data length.
     */

    r = fdc_send_byte(0xFF);

    if (r != FS_OK)
        return r;


    /*
     * В полноценной реализации здесь ждём IRQ6.
     *
     * Для первого теста просто ждём,
     * пока контроллер закончит busy.
     */

    uint32_t timeout = 2000000;

    while (timeout--)
    {
        uint8_t msr = inb(FDC_MSR);

        if (!(msr & FDC_MSR_CB))
            break;
    }

    if (!timeout)
        return FS_IO_ERROR;


    /*
     * Получаем 7 байт результата.
     */

    uint8_t result[7];

    for (int i = 0; i < 7; i++)
    {
        r = fdc_receive_byte(&result[i]);

        if (r != FS_OK)
            return r;
    }

    /*
     * ST0
     */

    if (result[0] & 0xC0)
        return FS_IO_ERROR;

    /*
     * ST1/ST2 errors.
     */

    if (result[1] & 0xBF)
        return FS_IO_ERROR;

    if (result[2] & 0x7F)
        return FS_IO_ERROR;

    return FS_OK;
}








/* ============================================================
 * Public sector read
 * ============================================================ */

int fat12_read_sectors(
        fat12_fs_t *vol,
        uint32_t lba,
        uint32_t count,
        void *buffer
)
{
    if (!vol || !buffer)
        return FS_IO_ERROR;

    //if (!vol->mounted) return FS_IO_ERROR;

    if (count == 0)
        return FS_OK;

    /*
     * Пока поддерживаем только стандартные
     * 512-байтовые сектора.
     */
    if (vol->bytes_per_sector != 512)
        return FS_NOT_SUPPORTED;

    uint8_t *dst = (uint8_t *)buffer;

    for (uint32_t i = 0; i < count; i++)
    {
        /*
         * ----------------------------------------------------
         * Читаем один физический сектор через FDC.
         * ----------------------------------------------------
         */

        uint32_t current_lba = lba + i;

        uint32_t sectors_per_cylinder =
            vol->heads * vol->sectors_per_track;

        uint32_t cylinder =
            current_lba / sectors_per_cylinder;

        uint32_t tmp =
            current_lba % sectors_per_cylinder;

        uint32_t head =
            tmp / vol->sectors_per_track;

        uint32_t sector =
            (tmp % vol->sectors_per_track) + 1;

        int r = fdc_read_sector(
            vol,
            cylinder,
            head,
            sector,
            dst + i * vol->bytes_per_sector
        );

        if (r != FS_OK)
            return r;
    }

    return FS_OK;
}


/* ============================================================
 * Write
 * ============================================================ */

int fat12_write_sectors(
    fat12_fs_t *vol,
    uint32_t lba,
    uint32_t count,
    const void *buffer
)
{
    (void)vol;
    (void)lba;
    (void)count;
    (void)buffer;

    /*
     * Пока только чтение.
     */

    return FS_NOT_SUPPORTED;
}


/* ============================================================
 * Disk initialization
 * ============================================================ */

int fat12_disk_init(
    fat12_fs_t *vol
)
{
    if (!vol)
        return FS_IO_ERROR;

    /*
     * Пока поддерживаем только floppy A:
     */

    if (vol->drive != 0)
        return FS_NOT_SUPPORTED;

    /*
     * Стандартная геометрия будет уточнена
     * после чтения BPB.
     *
     * Временно ставим 18 секторов/трек.
     */

    vol->sectors_per_track = 18;
    vol->heads = 2;

    /*
     * Сброс FDC.
     */

    int r = fdc_reset();

    if (r != FS_OK)
        return r;

    /*
     * Возвращаем головку на cylinder 0.
     */

    r = fdc_recalibrate();

    if (r != FS_OK)
        return r;

    return FS_OK;
}




uint16_t fat12_get_next_cluster(
    fat12_fs_t *vol,
    uint16_t cluster
)
{
    uint32_t offset;

    if (!vol)
        return 0xFFF;

    offset =
        cluster +
        (cluster / 2);

    if (offset + 1 >=
        vol->sectors_per_fat *
        vol->bytes_per_sector)
    {
        return 0xFFF;
    }

    uint16_t value =
        vol->fat[offset] |
        ((uint16_t)vol->fat[offset + 1] << 8);

    if (cluster & 1)
        value >>= 4;
    else
        value &= 0x0FFF;

    return value & 0x0FFF;
}



/*

int fat12_read_file(
    fat12_fs_t *vol,
    uint16_t first_cluster,
    uint32_t file_size,
    void *buffer
)
{
    if (!vol || !buffer)
        return FS_IO_ERROR;

    if (!vol->mounted)
        return FS_IO_ERROR;

    if (file_size == 0)
        return FS_OK;

    if (first_cluster < 2)
        return FS_CORRUPTED;


    uint8_t *dst =
        (uint8_t *)buffer;

    uint32_t remaining =
        file_size;

    uint32_t cluster_size =
        vol->sectors_per_cluster *
        vol->bytes_per_sector;

    uint16_t cluster =
        first_cluster;

    uint32_t cluster_count = 0;

    uint8_t sector_buffer[512];


    while (remaining > 0)
    {
        if (cluster < 2)
            return FS_CORRUPTED;

        if (cluster >= 0xFF0)
            return FS_CORRUPTED;


        uint32_t lba =
            vol->data_start +
            ((uint32_t)(cluster - 2) *
             vol->sectors_per_cluster);


        uint32_t bytes_this_cluster =
            remaining;

        if (bytes_this_cluster > cluster_size)
            bytes_this_cluster = cluster_size;


        uint32_t full_sectors =
            bytes_this_cluster /
            vol->bytes_per_sector;

        uint32_t tail =
            bytes_this_cluster %
            vol->bytes_per_sector;


        if (full_sectors > 0)
        {
            int r = fat12_read_sectors(
                vol,
                lba,
                full_sectors,
                dst
            );

            if (r != FS_OK)
                return r;

            dst +=
                full_sectors *
                vol->bytes_per_sector;
        }


        if (tail > 0)
        {
            int r = fat12_read_sectors(
                vol,
                lba + full_sectors,
                1,
                sector_buffer
            );

            if (r != FS_OK)
                return r;

            memcpy(
                dst,
                sector_buffer,
                tail
            );

            dst += tail;
        }


        remaining -=
            bytes_this_cluster;


        if (remaining == 0)
            return FS_OK;



        cluster =
            fat12_get_next_cluster(
                vol,
                cluster
            );


        if (cluster >= 0xFF8)
            return FS_CORRUPTED;

        if (cluster == 0xFF7)
            return FS_CORRUPTED;

        if (cluster == 0)
            return FS_CORRUPTED;


        cluster_count++;

        if (cluster_count > 4096)
            return FS_CORRUPTED;
    }

    return FS_OK;
}
*/
