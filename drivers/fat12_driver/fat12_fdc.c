#include "include/fat12_disk.h"
#include "include/fat12_internal.h"
//#include "include/fat12_io.h"
#include "include/fat12_fdc.h"
#include "include/fat12_dma.h"
#include "include/fat12_driver.h"
#include "../../include-kernel/lib.h"
#include "../../include-kernel/ports_io.h"
#include "../include_drivers/drv_format.h"



static void fdc_unlock_scheduler(void){
    if (api_ptr && api_ptr->schedule_lock_ptr) {
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 0;
    }
}

//wait
int fat12_fdc_wait_irq_async(uint32_t timeout_loops) {
    if (!api_ptr->floppy_irq_fired_ptr) return FS_IO_ERROR;
    volatile uint32_t loops = timeout_loops;
    while (loops--) {
        if (*(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr == 1){
            *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
            return FS_OK;
        }

        uint8_t msr = inb(FDC_MSR);
        if ((msr & FDC_MSR_RQM) && (msr & FDC_MSR_DIO)
            //&&(msr & FDC_MSR_BUSY)
        ){
            *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
            return FS_OK;
        }
        if (!(msr & FDC_MSR_BUSY)){
            *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
            return FS_OK;
        }

        // небольшая пауза, чтобы не забивать шину
        asm volatile("pause");
    }
    return FS_IO_ERROR;
}




//send command byte
int fdc_send_byte(uint8_t value){
    uint32_t timeout = 1000000;
    while (timeout--){
        uint8_t msr = inb(FDC_MSR);
        if ((msr & FDC_MSR_RQM) && !(msr & FDC_MSR_DIO)){
            outb(FDC_FIFO, value);
            return FS_OK;
        }
    }
    return FS_IO_ERROR;
}


//receive result byte
int fdc_receive_byte(uint8_t *value){
    uint32_t timeout = 1000000;
    while (timeout--){
        uint8_t msr = inb(FDC_MSR);
        // Ждем, пока контроллер будет готов (RQM = 1) и проверяем, что направление данных строго к процессору (DIO = 1)
        if ((msr & FDC_MSR_RQM) && (msr & FDC_MSR_DIO)){
            *value = inb(FDC_FIFO);
            return FS_OK;
        }
    }

    return FS_IO_ERROR;
}


//reset FDC
int fdc_reset(void){
    uint8_t st0;
    uint8_t cylinder;

    if (api_ptr && api_ptr->schedule_lock_ptr){
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 1;
    }
    //RESET = 0
    outb(FDC_DOR, 0x00);
    //Небольшая задержка Пока простой busy loop.
    for (volatile int i = 0; i < 2000000; i++) asm volatile ("nop");
    //RESET = 1 DMA enable drive 0
    outb(FDC_DOR,FDC_DOR_RESET | FDC_DOR_DMA_IRQ);

    for (int i = 0; i < 4; i++){
        // Отправляем команду
        if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) break;
        // Читаем первый байт (ST0)
        if (fdc_receive_byte(&st0) != FS_OK) break;
        // Читаем второй байт (Cylinder)
        if (fdc_receive_byte(&cylinder) != FS_OK) break;
    }
    outb(FDC_DOR, FDC_DOR_RESET | FDC_DOR_DMA_IRQ | FDC_DOR_MOTOR0);

    //Ждем физического раскручивания диска (около 300-500 мс)
    //Для 86Box это критично, иначе сектор не прочитается (будет ошибка старта)
    for (volatile int delay = 0; delay < 50000000; delay++) asm volatile("nop");

    *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
    //Команда RECALIBRATE (перемещение головки на трек 0)
    fdc_send_byte(0x07); // RECALIBRATE
    fdc_send_byte(0x00); // Drive 0

    if (fat12_fdc_wait_irq_async(40000000) != FS_OK) {
        fdc_unlock_scheduler();
        return FS_IO_ERROR; // Если таймаут —выходим с ошибкой механики
    }

    // Ждем завершения калибровки через SENSE_INTERRUPT
    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_receive_byte(&st0) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_receive_byte(&cylinder) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    if (cylinder != 0) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    fdc_unlock_scheduler();

    return FS_OK;
}


//motor
static void fdc_motor_on(void){
    outb(FDC_DOR, FDC_DOR_RESET | FDC_DOR_DMA_IRQ | FDC_DOR_MOTOR0);
}

//Recalibrate
int fdc_recalibrate(void){
    uint8_t st0;
    uint8_t cylinder;

    int r;
    if (api_ptr && api_ptr->schedule_lock_ptr){
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 1;
    }
    if (api_ptr->floppy_irq_fired_ptr){
        *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
    }

    r = fdc_send_byte(FDC_CMD_RECALIBRATE);
    if (r != FS_OK) {fdc_unlock_scheduler(); return r;}
    r = fdc_send_byte(0);
    if (r != FS_OK) {fdc_unlock_scheduler(); return r;}

    if (fat12_fdc_wait_irq_async(40000000) != FS_OK){
        fdc_unlock_scheduler();
        return FS_IO_ERROR; // Вылет по таймауту
    }

    r = fdc_sense_interrupt(&st0,&cylinder);
    if (r != FS_OK) {fdc_unlock_scheduler(); return r;}
    if (cylinder != 0) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    fdc_unlock_scheduler();

    return FS_OK;
}


int fdc_sense_interrupt(uint8_t *st0,uint8_t *cylinder){
    int r;
    r = fdc_send_byte(FDC_CMD_SENSE_INTERRUPT);
    if (r != FS_OK) return r;

    r = fdc_receive_byte(st0);
    if (r != FS_OK) return r;

    r = fdc_receive_byte(cylinder);
    if (r != FS_OK) return r;

    return FS_OK;
}






//Чтение одного сектора
//CHS:
//cylinder - 0..
//head     - 0/1
//sector   - 1..
//DMA:
//floppy -> DMA channel 2 -> FAT12_DMA_BUFFER
//После успешного чтения данные копируются из DMA-буфера в buffer
int fdc_read_sector(fat12_fs_t *vol,uint32_t cylinder,uint32_t head,uint32_t sector,void *buffer){
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

    uint8_t seek_st0, seek_cyl;

    if (!vol || !buffer) return FS_IO_ERROR;
    //Для FAT12 с нашими дискетами пока работаем только с drive 0
    if (vol->drive != 0) return FS_NOT_SUPPORTED;
    //Размер сектора. На этом этапе поддерживаем только 512 байт.
    if (vol->bytes_per_sector != 512) return FS_NOT_SUPPORTED;
    //CHS sanity check
    if (head >= vol->heads) return FS_IO_ERROR;
    if (sector == 0 || sector > vol->sectors_per_track) return FS_IO_ERROR;

    // Блокируем планировщик
    if (api_ptr && api_ptr->schedule_lock_ptr){
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 1;
    }

    // 1. Посылаем команду SEEK (0x0F)
    if (fdc_send_byte(0x0F) != FS_OK) return FS_IO_ERROR;
    // Передаем головку (сдвиг на 2 бита влево) и номер дисковода (0)
    if (fdc_send_byte((head << 2) | 0) != FS_OK) return FS_IO_ERROR;
    if (fdc_send_byte(cylinder) != FS_OK) return FS_IO_ERROR;

    // 2. Ждем окончания перемещения головки.
    // На реальном железе и в 86Box команда SEEK генерирует прерывание.
    // Так как мы работаем без IRQ-обработчика, мы обязаны сбросить его через SENSE_INTERRUPT:
    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) return FS_IO_ERROR;
    if (fdc_receive_byte(&seek_st0) != FS_OK) return FS_IO_ERROR;
    if (fdc_receive_byte(&seek_cyl) != FS_OK) return FS_IO_ERROR;

    // Проверяем, что поиск завершился успешно (в seek_st0 не должно быть битов ошибки)
    // Биты 5 (Seek End) должен быть 1, а биты 7 и 6 должны быть 0.
    if ((seek_st0 & 0xC0) != 0) return FS_IO_ERROR;


    if (fdc_send_byte(FDC_CMD_SPECIFY) != FS_OK) return FS_IO_ERROR;
    if (fdc_send_byte(0xDF) != FS_OK) return FS_IO_ERROR; // SRT/HUT
    if (fdc_send_byte(0x02) != FS_OK) return FS_IO_ERROR;

    // Небольшая задержка, чтобы механика «успокоилась» (актуально для эмуляторов)
    for (volatile int delay = 0; delay < 200000; delay++) asm volatile("nop");

    //Motor
    fdc_motor_on();
    //outb(FDC_DOR, FDC_DOR_RESET | FDC_DOR_DMA_IRQ | FDC_DOR_MOTOR0 | FDC_DOR_DRIVE0);
    io_wait(); io_wait(); io_wait();

    //DMA
    //Настраиваем channel 2 на чтение одного сектора.
    result = fat12_dma_read((void *)FAT12_DMA_BUFFER,512);
    if (result != FS_OK) return result;

    io_wait(); io_wait(); io_wait();

    if (api_ptr->floppy_irq_fired_ptr) {
        *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
    }
    //READ DATA
    //Команда:
    //0: 46h + MFM bit
    //1: HD + drive
    //2: cylinder
    //3: head
    //4: sector
    //5: sector size
    //6: end of track
    //7: gap
    //8: data length

    result = fdc_send_byte(FDC_CMD_READ_DATA);

    if (result != FS_OK) return result;

    //Первый параметр:
    //bit 2 = head
    //bits 0..1 = drive
    //Для drive 0:
    //head 0 -> 00
    //head 1 -> 04

    result = fdc_send_byte((uint8_t)((head << 2) | 0));
    if (result != FS_OK) return result;

    c = (uint8_t)cylinder;
    h = (uint8_t)head;
    r = (uint8_t)sector;

    //N = 2 означает 512 байт
    n = 2;

    //Читаем только один сектор
    //eot = r;
    eot = (vol && vol->sectors_per_track > 0) ? (uint8_t)vol->sectors_per_track : 18;

    //Стандартный gap для 3.5"/5.25" double-density
    gpl = 0x1B;
    //Для N=2 значение DTL игнорируется
    dtl = 0xFF;

    result = fdc_send_byte(c);
    if (result != FS_OK) return result;

    result = fdc_send_byte(h);
    if (result != FS_OK) return result;

    result = fdc_send_byte(r);
    if (result != FS_OK) return result;

    result = fdc_send_byte(n);
    if (result != FS_OK) return result;

    result = fdc_send_byte(eot);
    if (result != FS_OK) return result;

    result = fdc_send_byte(gpl);
    if (result != FS_OK) return result;

    result = fdc_send_byte(dtl);
    if (result != FS_OK) return result;

    //Ждём result phase, ожидаем флаг установленного, которое ядро ловит IRQ6.
    if (api_ptr->floppy_irq_fired_ptr) {
            *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
    }

    uint32_t data_phase_timeout = 20000000;
    while (data_phase_timeout--) {
        uint8_t msr = inb(FDC_MSR);
        // RQM = 1, DIO = 1 (Контроллер готов отдавать байты результатов)
        if ((msr & FDC_MSR_RQM) && (msr & FDC_MSR_DIO)) {
            break;
        }
        __asm__ __volatile__("pause");
    }

    if (data_phase_timeout == 0) {
        *(volatile uint8_t *)api_ptr->floppy_irq_fired_ptr = 0;
        return FS_IO_ERROR; // Таймаут передачи данных
    }

    //Result phase
    //READ DATA возвращает 7 байт:
    //ST0
    //ST1
    //ST2
    //C
    //H
    //R
    //N
    result = fdc_receive_byte(&st0);
    if (result != FS_OK) return result;

    result = fdc_receive_byte(&st1);
    if (result != FS_OK) return result;

    result = fdc_receive_byte(&st2);
    if (result != FS_OK) return result;

    result = fdc_receive_byte(&c);
    if (result != FS_OK) return result;

    result = fdc_receive_byte(&h);
    if (result != FS_OK) return result;

    result = fdc_receive_byte(&r);
    if (result != FS_OK) return result;

    result = fdc_receive_byte(&n);
    if (result != FS_OK) return result;

    //Проверяем ошибки FDC.
    if (st0 & 0xC0) return FS_IO_ERROR;
    if (st1 != 0) return FS_IO_ERROR;
    if (st2 != 0) return FS_IO_ERROR;

    //Пока не проверяем CHRN. Нужно убедиться, что DMA действительно получил содержимое boot sector
    if (n != 2) return FS_IO_ERROR;

    //Копируем данные из DMA buffer
    memcpy(buffer,(void *)FAT12_DMA_BUFFER,512);

    // Разблокируем планировщик
    fdc_unlock_scheduler();

    return FS_OK;
}


int fdc_write_sector(fat12_fs_t *vol,uint32_t cylinder,uint32_t head,uint32_t sector,const void *buffer){
    uint8_t st0, st1, st2, c, h, r, n;
    uint8_t seek_st0, seek_cyl;

    if (!vol || !buffer || vol->drive != 0) return FS_IO_ERROR;

    // Блокируем планировщик
    if (api_ptr && api_ptr->schedule_lock_ptr) {
        *(volatile uint8_t *)api_ptr->schedule_lock_ptr = 1;
    }

    //Позиционирование головки (SEEK)
    if (fdc_send_byte(FDC_CMD_SEEK) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_send_byte((head << 2) | 0) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_send_byte(cylinder) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    if (fdc_send_byte(FDC_CMD_SENSE_INTERRUPT) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_receive_byte(&seek_st0) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_receive_byte(&seek_cyl) != FS_OK) {fdc_unlock_scheduler();return FS_IO_ERROR;}
    if ((seek_st0 & 0xC0) != 0) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    for (volatile int delay = 0; delay < 200000; delay++) asm volatile("nop");

    //Подготовка данных в DMA буфере
    memcpy((void *)FAT12_DMA_BUFFER, buffer, 512);

    //Настройка DMA на запись
    if (fat12_dma_write((void *)FAT12_DMA_BUFFER, 512) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    //Врубаем мотор
    fdc_motor_on();
    io_wait(); io_wait(); io_wait();

    //Команда WRITE DATA (0x45)
    if (fdc_send_byte(FDC_CMD_WRITE_DATA) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_send_byte((head << 2) | 0) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    c = (uint8_t)cylinder;
    h = (uint8_t)head;
    r = (uint8_t)sector;
    n = 2; // 512 байт
    uint8_t write_eot = (vol && vol->sectors_per_track > 0) ? (uint8_t)vol->sectors_per_track : 18;
    if (fdc_send_byte(c) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_send_byte(h) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_send_byte(r) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_send_byte(n) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}
    if (fdc_send_byte(write_eot) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    if (fdc_send_byte(0x1B) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} // GPL
    if (fdc_send_byte(0xFF) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} // DTL

    // Опрос завершения фазы (Polling Result)
    uint32_t timeout = 10000000;
    while (timeout--){
        uint8_t msr = inb(FDC_MSR);
        if ((msr & FDC_MSR_RQM) && (msr & FDC_MSR_DIO)) break;
    }
    if (!timeout) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    // Читаем 7 байт статуса
    if (fdc_receive_byte(&st0) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} io_wait();
    if (fdc_receive_byte(&st1) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} io_wait();
    if (fdc_receive_byte(&st2) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} io_wait();
    if (fdc_receive_byte(&c) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} io_wait();
    if (fdc_receive_byte(&h) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} io_wait();
    if (fdc_receive_byte(&r) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} io_wait();
    if (fdc_receive_byte(&n) != FS_OK) {fdc_unlock_scheduler(); return FS_IO_ERROR;} io_wait();


    if (st0 & 0xC0) {fdc_unlock_scheduler(); return FS_IO_ERROR;} // Крах или защита от записи (Write Protected)
    if (st1 != 0 || st2 != 0 || n != 2) {fdc_unlock_scheduler(); return FS_IO_ERROR;}

    //Разблокируем планировщик
    fdc_unlock_scheduler();

    return FS_OK;
}

