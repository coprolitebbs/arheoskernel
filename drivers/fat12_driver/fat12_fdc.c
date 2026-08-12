
#include "include/fat12_disk.h"
#include "include/fat12_internal.h"
#include "include/fat12_io.h"
#include "include/fat12_fdc.h"
#include "include/fat12_dma.h"
#include "../../include-kernel/lib.h"
#include "../include_drivers/drv_format.h"


/*
 * ============================================================
 * Intel 82077 / NEC 765 compatible floppy controller
 * ============================================================
 */

#define FDC_DOR        0x3F2
#define FDC_MSR        0x3F4
#define FDC_FIFO       0x3F5
#define FDC_CTRL       0x3F7


/*
 * Digital Output Register
 */

#define FDC_DOR_DRIVE0 0x00
#define FDC_DOR_RESET  0x04
#define FDC_DOR_DMA    0x08
#define FDC_DOR_MOTOR0 0x10


/*
 * Main Status Register
 */

#define FDC_MSR_BUSY   0x10
#define FDC_MSR_DIO    0x40
#define FDC_MSR_RQM    0x80


/*
 * Commands
 */

//#define FDC_CMD_READ_DATA      0x06
#define FDC_CMD_READ_DATA 0x46
#define FDC_CMD_SENSE_INTERRUPT 0x08
#define FDC_CMD_SPECIFY        0x03
#define FDC_CMD_RECALIBRATE    0x07
#define FDC_CMD_SEEK           0x0F


/*
 * ------------------------------------------------------------
 * I/O
 * ------------------------------------------------------------
 *
 * Пока локальные функции.
 * Позже можем вынести их в общий kernel I/O API.
 */
/*
static inline void outb(uint16_t port, uint8_t value)
{
    asm volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}


static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    asm volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}
*/

/*
 * ------------------------------------------------------------
 * wait
 * ------------------------------------------------------------
 */

static int fdc_wait_rqm(uint32_t timeout)
{
    while (timeout--)
    {
        uint8_t msr = inb(FDC_MSR);

        if (msr & FDC_MSR_RQM)
            return FS_OK;
    }

    return FS_IO_ERROR;
}


/*
 * ------------------------------------------------------------
 * send command byte
 * ------------------------------------------------------------
 */
int fdc_send_byte(uint8_t value)
{
    if (fdc_wait_rqm(1000000) != FS_OK)
        return FS_IO_ERROR;

    /*
     * Передача команды:
     *
     * RQM = 1
     * DIO = 0
     */

    if (inb(FDC_MSR) & FDC_MSR_DIO)
        return FS_IO_ERROR;

    outb(FDC_FIFO, value);

    return FS_OK;
}


/*
 * ------------------------------------------------------------
 * receive result byte
 * ------------------------------------------------------------
 */

int fdc_receive_byte(uint8_t *value)
{
    if (fdc_wait_rqm(1000000) != FS_OK)
        return FS_IO_ERROR;

    /*
     * DIO должен быть установлен:
     * контроллер передаёт данные процессору.
     */

    if (!(inb(FDC_MSR) & FDC_MSR_DIO))
        return FS_IO_ERROR;

    *value = inb(FDC_FIFO);

    return FS_OK;
}


/*
 * ------------------------------------------------------------
 * reset FDC
 * ------------------------------------------------------------
 */
int fdc_reset(void)
{
    uint8_t st0;
    uint8_t cylinder;

    /*
     * RESET = 0
     */

    outb(FDC_DOR, 0x00);

    /*
     * Небольшая задержка.
     *
     * Пока простой busy loop.
     */

    for (volatile int i = 0; i < 100000; i++)
        asm volatile ("nop");


    /*
     * RESET = 1
     * DMA enable
     * drive 0
     */

    outb(
        FDC_DOR,
        FDC_DOR_RESET | FDC_DOR_DMA
    );


    /*
     * После reset FDC выдаёт четыре
     * interrupt condition.
     *
     * Читаем их через SENSE INTERRUPT.
     */

    for (int i = 0; i < 4; i++)
    {
        if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK)
            return FS_IO_ERROR;

        if (fdc_receive_byte(&st0) != FS_OK)
            return FS_IO_ERROR;

        if (fdc_receive_byte(&cylinder) != FS_OK)
            return FS_IO_ERROR;
    }

    return FS_OK;
}


/*
 * ------------------------------------------------------------
 * motor
 * ------------------------------------------------------------
 */

static void fdc_motor_on(void)
{
    outb(
        FDC_DOR,
        FDC_DOR_RESET |
        FDC_DOR_DMA |
        FDC_DOR_MOTOR0
    );
}

/* ============================================================
 * Recalibrate
 * ============================================================ */

int fdc_recalibrate(void)
{
    uint8_t st0;
    uint8_t cylinder;

    int r;

    r = fdc_send_byte(FDC_CMD_RECALIBRATE);

    if (r != FS_OK)
        return r;

    r = fdc_send_byte(0);

    if (r != FS_OK)
        return r;

    uint32_t timeout = 2000000;

    while (timeout--)
    {
        uint8_t msr = inb(FDC_MSR);

        /*
        * CB = Controller Busy.
        *
        * Пока CB установлен,
        * команда ещё выполняется.
        */

        if (!(msr & FDC_MSR_BUSY))
            break;
    }

    if (!timeout)
        return FS_IO_ERROR;

        /*
        * В полноценном варианте здесь ждём IRQ6.
        *
        * Пока предполагаем, что контроллер успевает
        * выполнить операцию.
        */

        r = fdc_sense_interrupt(
            &st0,
            &cylinder
        );

        if (r != FS_OK)
            return r;

        if (cylinder != 0)
            return FS_IO_ERROR;

        return FS_OK;
    }



int fdc_sense_interrupt(
    uint8_t *st0,
    uint8_t *cylinder
)
{
    int r;

    r = fdc_send_byte(FDC_CMD_SENSE_INTERRUPT);

    if (r != FS_OK)
        return r;

    r = fdc_receive_byte(st0);

    if (r != FS_OK)
        return r;

    r = fdc_receive_byte(cylinder);

    if (r != FS_OK)
        return r;

    return FS_OK;
}





/*
 * ============================================================
 * Read one sector
 * ============================================================
 *
 * CHS:
 *
 * cylinder - 0..
 * head     - 0/1
 * sector   - 1..
 *
 * DMA:
 *   floppy -> DMA channel 2 -> FAT12_DMA_BUFFER
 *
 * После успешного чтения данные копируются
 * из DMA-буфера в buffer.
 *
 * ============================================================
 */

int fdc_read_sector(
    fat12_fs_t *vol,
    uint32_t cylinder,
    uint32_t head,
    uint32_t sector,
    void *buffer
)
{
    uint8_t st0;
    uint8_t st1;
    uint8_t st2;
    uint8_t c;
    uint8_t h;
    uint8_t r;
    uint8_t n;
    uint8_t eot;
    uint8_t gpl;
    uint8_t dtl;

    int result;


    if (!vol || !buffer)
        return FS_IO_ERROR;


    /*
     * Для FAT12 с нашими дискетами
     * пока работаем только с drive 0.
     */

    if (vol->drive != 0)
        return FS_NOT_SUPPORTED;


    /*
     * Размер сектора.
     *
     * На этом этапе поддерживаем только 512 байт.
     */

    if (vol->bytes_per_sector != 512)
        return FS_NOT_SUPPORTED;


    /*
     * CHS sanity check.
     */

    if (head >= vol->heads)
        return FS_IO_ERROR;

    if (sector == 0 ||
        sector > vol->sectors_per_track)
        return FS_IO_ERROR;


   /*
     * --------------------------------------------------------
     * DMA
     * --------------------------------------------------------
     *
     * Настраиваем channel 2 на чтение
     * одного сектора.
     */

    result = fat12_dma_read(
        (void *)FAT12_DMA_BUFFER,
        512
    );

    if (result != FS_OK)
        return result;


    /*
     * --------------------------------------------------------
     * Motor
     * --------------------------------------------------------
     */

    fdc_motor_on();


    /*
     * --------------------------------------------------------
     * READ DATA
     * --------------------------------------------------------
     *
     * Команда:
     *
     * 0: 46h + MFM bit
     * 1: HD + drive
     * 2: cylinder
     * 3: head
     * 4: sector
     * 5: sector size
     * 6: end of track
     * 7: gap
     * 8: data length
     */

    result = fdc_send_byte(
        FDC_CMD_READ_DATA
    );

    if (result != FS_OK)
        return result;


    /*
     * Первый параметр:
     *
     * bit 2 = head
     * bits 0..1 = drive
     *
     * Для drive 0:
     *
     * head 0 -> 00
     * head 1 -> 04
     */

    result = fdc_send_byte(
        (uint8_t)((head << 2) | 0)
    );

    if (result != FS_OK)
        return result;


    c = (uint8_t)cylinder;
    h = (uint8_t)head;
    r = (uint8_t)sector;

    /*
     * N = 2 означает 512 байт.
     */

    n = 2;

    /*
     * Читаем только один сектор.
     */

    eot = r;

    /*
     * Стандартный gap для 3.5"/5.25"
     * double-density.
     */

    gpl = 0x1B;

    /*
     * Для N=2 значение DTL
     * игнорируется.
     */

    dtl = 0xFF;


    result = fdc_send_byte(c);

    if (result != FS_OK)
        return result;

    result = fdc_send_byte(h);

    if (result != FS_OK)
        return result;

    result = fdc_send_byte(r);

    if (result != FS_OK)
        return result;

    result = fdc_send_byte(n);

    if (result != FS_OK)
        return result;

    result = fdc_send_byte(eot);

    if (result != FS_OK)
        return result;

    result = fdc_send_byte(gpl);

    if (result != FS_OK)
        return result;

    result = fdc_send_byte(dtl);

    if (result != FS_OK)
        return result;


    /*
     * --------------------------------------------------------
     * Ждём result phase.
     * --------------------------------------------------------
     *
     * В полноценном варианте здесь должен быть IRQ6.
     *
     * Пока polling.
     */

    uint32_t timeout = 10000000;

    while (timeout--)
    {
        uint8_t msr = inb(FDC_MSR);

        /*
         * RQM = 1
         * DIO = 1
         *
         * Контроллер готов отдавать
         * result bytes.
         */

        if ((msr & FDC_MSR_RQM) &&
            (msr & FDC_MSR_DIO)
            //&&(msr & FDC_MSR_BUSY)
        )
        {
            break;
        }
    }

    if (!timeout)
        return FS_IO_ERROR;


    /*
     * --------------------------------------------------------
     * Result phase
     *
     * READ DATA возвращает 7 байт:
     *
     * ST0
     * ST1
     * ST2
     * C
     * H
     * R
     * N
     * --------------------------------------------------------
     */

    result = fdc_receive_byte(&st0);

    if (result != FS_OK)
        return result;

    result = fdc_receive_byte(&st1);

    if (result != FS_OK)
        return result;

    result = fdc_receive_byte(&st2);

    if (result != FS_OK)
        return result;

    result = fdc_receive_byte(&c);

    if (result != FS_OK)
        return result;

    result = fdc_receive_byte(&h);

    if (result != FS_OK)
        return result;

    result = fdc_receive_byte(&r);

    if (result != FS_OK)
        return result;

    result = fdc_receive_byte(&n);

    if (result != FS_OK)
        return result;

    /*
     * --------------------------------------------------------
     * Проверяем ошибки FDC.
     * --------------------------------------------------------
     */

    if (st0 & 0xC0)
        return FS_IO_ERROR;

    if (st1 != 0)
        return FS_IO_ERROR;

    if (st2 != 0)
        return FS_IO_ERROR;


    /*
     * Проверяем, что контроллер действительно
     * прочитал тот сектор, который просили.
     */
    /*
    if (c != (uint8_t)cylinder)
        return FS_IO_ERROR;

    if (h != (uint8_t)head)
        return FS_IO_ERROR;

    if (r != (uint8_t)sector)
        return FS_IO_ERROR;
    */

    /*
    * Пока не проверяем CHRN.
    * Нам важно убедиться, что DMA действительно
    * получил содержимое boot sector.
    */
    if (n != 2)
        return FS_IO_ERROR;


    /*
     * --------------------------------------------------------
     * Копируем данные из DMA buffer.
     * --------------------------------------------------------
     */

    memcpy(
        buffer,
        (void *)FAT12_DMA_BUFFER,
        512
    );


    return FS_OK;
}
