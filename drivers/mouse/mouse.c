/*
 * Copyright (C) 2026 ARHeOS by Dmitry H
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */


// mouse.c — драйвер указательных устройств (мышь) ARHeOS
//
// Архитектура (модель как у Windows):
//   * Драйвер держит реестр провайдеров (PS/2 сейчас, в будущем USB-HID).
//     Каждый провайдер регистрируется через register_device() и отдает дельты
//     через свой poll_fn(). Все провайдеры двигают один курсор: их дельты
//     суммируются, кнопки объединяются по OR от текущего состояния каждого.
//   * Дефолтный провайдер — PS/2 AUX (i8042, IRQ12).
//   * Единый курсор живет внутри самого драйвера; ядро только дергает
//     api->poll() из schedule() и из системных вызовов SYS_MOUSE_*.
//
// Детект и опрос полностью poll-овые: читаем статус-регистр i8042 (0x64)
// и данные (0x60) напрямую. IRQ12 нужен только как будильник для будущей
// событийной модели, его отсутствие (замаскированный PIC, не пришитый isr.c)
// не мешает ни детекту, ни работе
//
// Порядок чтения:
//   - байты мыши отличаем от клавиатурных по биту AUX (bit5 статуса)
//   - чужие (клавиатурные) байты вычитываем и выбрасываем, чтобы ACK
//     мыши не потерялся за ними в очереди
//   - перед каждой записью в контроллер ждем IBF==0
//   - команды мыши идут только через префикс 0xD4
//   - на время инициализации клавиатурный порт отключается (0xAD/0xAE),
//     чтобы пользователь, печатающий в момент загрузки, не вклинил
//     клавиатурный байт в диалог с мышью
//   - таймауты большие: реальное железо отвечает десятки миллисекунд
//
// Конвенция дельт: провайдер отдает dy уже в экранных координатах
// (dy>0 — курсор идет вниз). PS/2 возвращает dy в стиле "вверх — это плюс",
// поэтому провайдер инвертирует знак. Аккумулятор знаки не трогает.
//
// Контракт poll_fn провайдера: заполняет out при каждом вызове (в том числе
// при отсутствии новых пакетов отдает последнее известное состояние кнопок)
// и возвращает 1, если была новая активность, 0 — если тихо.


#include "../include_drivers/drv_format.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/ports_io.h"
#include "../../include-kernel/lib.h"

// Внутреннее состояние PS/2-провайдера

#define I8042_STATUS   0x64
#define I8042_DATA     0x60

#define ST_OBF         0x01   //в выходном буфере есть байт
#define ST_IBF         0x02   //входной буфер занят — писать нельзя
#define ST_AUX         0x20   //байт в выходном буфере — от AUX (мыши)

//Таймауты в итерациях опроса. Одна итерация = inb + io_wait (единицы мкс
//даже на Geode GX1). QEMU отвечает мгновенно, реальное железо — до сотен мс
#define TIMEOUT_FAST   200000    //ожидание ACK (~0.2-1 с на реальном железе)
#define TIMEOUT_RESET  1000000   //ожидание 0xAA после Reset (~1-3 с)
#define TIMEOUT_CCB    200000

//Диагностика детекта: на каком шаге инициализация остановилась (MPST_*)
//и какой байт был последним принят на этом шаге (для строк "no 0xAA, got XX")
static uint32_t init_step    = MPST_OK;
static int32_t  init_last_b  = -1;     //0..255 или -1 (тишина)

static uint8_t  ps2_last_btns = 0;     //последнее состояние кнопок PS/2-мыши

//Накопитель пакетов: байты приходят асинхронно (по IRQ или между опросами),
//поэтому собираем 3-байтовый пакет постепенно
static uint8_t  ps2_pkt[3];
static uint8_t  ps2_pkt_idx   = 0;

//Накопленная дельта с последнего вызова poll_fn (пакетов между опросами
//может прийти несколько — все складываем)
static int32_t  ps2_acc_dx    = 0;
static int32_t  ps2_acc_dy    = 0;

// Счетчики для отладки (видны в api->debug*)
static uint32_t dbg_packets   = 0;
static uint32_t dbg_resync    = 0;
static uint32_t dbg_overflow  = 0;

//Низкоуровневые примитивы i8042

//Ждать возможности записи (IBF==0). 1 — ок, 0 — таймаут.
static int i8042_wait_write(int loops)
{
    while (loops--)
    {
        if (!(inb(I8042_STATUS) & ST_IBF)) return 1;
        io_wait();
    }
    return 0;
}

//Вычитать все, что лежит в выходном буфере (без разбора). Нужно перед
//инициализацией: BIOS мог оставить там байты BAT-теста клавиатуры, и без
//flush первый ACK мыши оказался бы чужим байтом.
static void i8042_flush(int loops)
{
    while (loops--)
    {
        if (!(inb(I8042_STATUS) & ST_OBF)) return;
        (void)inb(I8042_DATA);
        io_wait();
    }
}

//Прочитать один байт мыши: ждем OBF, но забираем только байты со взведенным
//битом AUX. Клавиатурные байты (AUX==0) вычитываем и выбрасываем — иначе
//они навсегда застревают перед нашим ACK и все ломают.
//Возвращает 0..255 или -1 по таймауту.
static int ps2_read_byte(int loops)
{
    while (loops--)
    {
        uint8_t st = inb(I8042_STATUS);
        if (st & ST_OBF)
        {
            uint8_t b = inb(I8042_DATA);
            if (st & ST_AUX) return b;
            //чужой (клавиатурный) байт — проглотили, продолжаем ждать наш
        }
        io_wait();
    }
    return -1;
}

//Отправить команду мыши (через префикс 0xD4) и дождаться ACK 0xFA.
//Возвращает 1 при успехе.
static int ps2_command(uint8_t cmd)
{
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_STATUS, 0xD4);          // следующий байт в 0x60 — для AUX
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_DATA, cmd);

    int ack = ps2_read_byte(TIMEOUT_FAST);
    return (ack == 0xFA) ? 1 : 0;
}

// Инициализация PS/2 AUX. Полностью poll-овая: работает даже если IRQ12
// замаскирован в PIC или обработчик в isr.c еще не пришит
// Возвращает 1, если мышь найдена, 0 — если нет.
//При неудаче init_step хранит код шага (MPST_*), по которому ядро печатает причину на экране
static int mouse_ps2_init(void)
{
    uint8_t ccb;
    int resp, attempt, kb_disabled = 0;
    int ok = 0;

    ps2_pkt_idx = 0;
    ps2_acc_dx = 0;
    ps2_acc_dy = 0;
    ps2_last_btns = 0;
    init_step = MPST_OK;
    init_last_b = -1;

    //0. Отключить клавиатурный порт (0xAD) и очистить мусор в выходном буфере
    //(байты от BIOS/клавиатуры)
    //Без отключения печатающий пользователь мог бы вклинить клавиатурный байт в диалог с мышью
    if (!i8042_wait_write(TIMEOUT_FAST))
    {
        init_step = MPST_WR_TIMEOUT;
        goto done;
    }
    outb(I8042_STATUS, 0xAD);          //disable keyboard port
    kb_disabled = 1;
    i8042_flush(TIMEOUT_FAST);

    //1. Отключить и включить AUX-порт: приводит контроллер в заведомое
    //состояние (на 86box без этого AUX иногда спит)
    if (!i8042_wait_write(TIMEOUT_FAST))
    {
        init_step = MPST_WR_TIMEOUT;
        goto done;
    }
    outb(I8042_STATUS, 0xA7);          //disable AUX
    i8042_flush(TIMEOUT_FAST);
    if (!i8042_wait_write(TIMEOUT_FAST))
    {
        init_step = MPST_WR_TIMEOUT;
        goto done;
    }
    outb(I8042_STATUS, 0xA8);          //enable AUX
    i8042_flush(TIMEOUT_FAST);

    //2. Байт конфигурации контроллера (CCB):
    //      bit1 = разрешить IRQ12 от AUX
    //      bit5 = 0 (разрешить тактирование AUX-порта)
    //Ответ на 0x20 приходит как обычный байт контроллера (AUX==0),
    //читаем его именно контроллерным способом; клавиатура уже отключена,
    //так что бит AUX==0 здесь значит именно байт контроллера
    if (!i8042_wait_write(TIMEOUT_FAST))
    {
        init_step = MPST_WR_TIMEOUT;
        goto done;
    }
    outb(I8042_STATUS, 0x20);          //почитать Controller Configuration Byte
    {
        int t = TIMEOUT_CCB;
        resp = -1;
        while (t--)
        {
            uint8_t st = inb(I8042_STATUS);
            if (st & ST_OBF)
            {
                resp = inb(I8042_DATA);
                break;
            }
            io_wait();
        }
    }
    if (resp < 0)
    {
        init_step = MPST_NO_CCB;       //контроллер не ответил — AUX нет
        goto done;
    }
    init_last_b = resp;
    ccb = (uint8_t)resp;
    ccb |=  (1 << 1);                  //enable AUX IRQ12
    ccb &= ~(1 << 5);                  //enable AUX clock
    if (!i8042_wait_write(TIMEOUT_FAST))
    {
        init_step = MPST_WR_TIMEOUT;
        goto done;
    }
    outb(I8042_STATUS, 0x60);          //записать Controller Configuration Byte
    if (!i8042_wait_write(TIMEOUT_FAST))
    {
        init_step = MPST_WR_TIMEOUT;
        goto done;
    }
    outb(I8042_DATA, ccb);

    //3. Reset мыши: 0xFF -> ACK 0xFA, потом 0xAA (self-test ok) и 0x00 (id)
    //0xFC вместо 0xAA — ошибка самотеста, тишина — устройства нет.
    //Блок 3-6 делаем дважды: часть контроллеров теряет первый Reset, если
    //между 0xA8 и командой прошло слишком мало времени (наблюдалось на 86box)
    for (attempt = 0; attempt < 2 && !ok; attempt++)
    {
        i8042_flush(TIMEOUT_FAST);     //хвосты предыдущей попытки (AA/00/ACK)

        if (!ps2_command(0xFF))
        {
            init_step = MPST_NO_ACK;
            continue;
        }
        resp = ps2_read_byte(TIMEOUT_RESET);       //0xAA
        init_last_b = resp;
        if (resp != 0xAA)
        {
            init_step = MPST_NO_BT;
            continue;
        }
        (void)ps2_read_byte(TIMEOUT_RESET);        //device id (0x00), не проверяем жестко
        init_step = MPST_OK;
        init_last_b = -1;

        //4. Заводские настройки по умолчанию
        if (!ps2_command(0xF6))
        {
            init_step = MPST_NO_F6;
            continue;
        }

        //5. Частота опроса 40 отчетов/с (стабильность на реальном железе;
        //    заодно задел под будущую магию включения колеса)
        if (!ps2_command(0xF3) || !ps2_command(0x28))
        {
            init_step = MPST_NO_F3;
            continue;
        }

        //6. Разрешить стриминг отчетов
        if (!ps2_command(0xF4))
        {
            init_step = MPST_NO_F4;
            continue;
        }
        ok = 1;
    }

done:
    //Хвост общий для всех выходов: слить остатки и вернуть клавиатуру (0xAE).
    //Раньше при неудаче после шага 0 ядро оставалось без клавиатуры до ребута
    i8042_flush(TIMEOUT_FAST);
    if (kb_disabled && i8042_wait_write(TIMEOUT_FAST))
        outb(I8042_STATUS, 0xAE);      //enable keyboard port

    return ok;
}

// poll_fn провайдера PS/2: вычитывает все, что лежит в буфере, собирает
// 3-байтовые пакеты, накапливает дельту. Возвращает 1, если была активность
static int mouse_ps2_poll(void *priv, mouse_delta_t *out)
{
    (void)priv;

    //Вычитываем все, что есть сейчас (не ждем: poll — неблокирующий)
    for (;;)
    {
        uint8_t st = inb(I8042_STATUS);
        if (!(st & ST_OBF)) break;

        uint8_t b = inb(I8042_DATA);
        if (!(st & ST_AUX)) continue;           //клавиатурный байт — мимо

        if (ps2_pkt_idx == 0 && !(b & 0x08))
        {
            //байт синхронизации: bit3 первого байта пакета всегда 1
            //Если нет — мы потеряли границу пакета: выбрасываем байт
            dbg_resync++;
            continue;
        }

        ps2_pkt[ps2_pkt_idx++] = b;
        if (ps2_pkt_idx < 3) continue;
        ps2_pkt_idx = 0;

        if (ps2_pkt[0] & 0xC0)                  //overflow x/y — пакет битый
        {
            dbg_overflow++;
            continue;
        }

        int dx = ps2_pkt[1];
        int dy = ps2_pkt[2];
        if (ps2_pkt[0] & 0x10) dx -= 256;       //знак X (9-битное дополнение)
        if (ps2_pkt[0] & 0x20) dy -= 256;       //знак Y
        dy = -dy;                               //PS/2: плюс = вверх -> на экране вниз

        ps2_acc_dx += dx;
        ps2_acc_dy += dy;
        ps2_last_btns = ps2_pkt[0] & 0x07;      //bit0=L bit1=R bit2=M
        dbg_packets++;
    }

    if (out)
    {
        out->dx = (int16_t)ps2_acc_dx;
        out->dy = (int16_t)ps2_acc_dy;
        out->wheel = 0;
        out->buttons = ps2_last_btns;           //состояние кнопок отдаем всегда
    }
    int had_motion = (ps2_acc_dx != 0 || ps2_acc_dy != 0);
    ps2_acc_dx = 0;
    ps2_acc_dy = 0;
    return had_motion ? 1 : 0;
}

// Реестр провайдеров (сейчас: ps2, в будущем usbhid0 и т.п.)

static mouse_dev_entry_t mouse_devices[MAX_POINT_DEVICES];
static uint8_t mouse_dev_count = 0;

// Аккумулятор единого курсора (модель Windows)

static int32_t cur_x = 0, cur_y = 0;
static uint8_t cur_buttons = 0;
static int32_t acc_dx = 0, acc_dy = 0;    //дельта с предыдущего api->poll()
static uint32_t scr_w = 640, scr_h = 480;

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

//Публичный API (mouse_driver_api_t)

//Единый опрос: суммирует дельты всех провайдеров, двигает курсор,
//возвращает итоговое состояние. Вызывается из SYS_MOUSE_* и из schedule()
static int mouse_api_poll(mouse_state_t *out)
{
    //Обнуленная дельта: провайдер обязан заполнить out (контракт выше),
    //но нулевая инициализация страхует от будущего провайдера с багом
    mouse_delta_t d = {0, 0, 0, 0};
    int i;

    acc_dx = 0;
    acc_dy = 0;

    //Кнопки пересчитываем с нуля на каждом опросе: каждый провайдер отдает
    //полное текущее состояние своих кнопок (контракт poll_fn), так что
    //отпускание кнопки доходит до курсора, а не прилипает навсегда
    cur_buttons = 0;

    for (i = 0; i < MAX_POINT_DEVICES; i++)
    {
        if (!mouse_devices[i].used) continue;
        if (!mouse_devices[i].poll_fn) continue;
        mouse_devices[i].poll_fn(mouse_devices[i].priv, &d);
        acc_dx += d.dx;
        acc_dy += d.dy;
        cur_buttons |= d.buttons;      //модель Windows: кнопка нажата, если ее держит любое из устройств
    }

    cur_x = clampi(cur_x + acc_dx, 0, (int32_t)scr_w - 1);
    cur_y = clampi(cur_y + acc_dy, 0, (int32_t)scr_h - 1);

    if (out)
    {
        out->x = (int16_t)cur_x;
        out->y = (int16_t)cur_y;
        out->dx = (int16_t)acc_dx;
        out->dy = (int16_t)acc_dy;
        out->buttons = cur_buttons;
        out->device_count = mouse_dev_count;
    }
    return 0;                          //0 = ок (курсор всегда валиден)
}

static int mouse_api_set_pos(int16_t x, int16_t y)
{
    cur_x = clampi(x, 0, (int32_t)scr_w - 1);
    cur_y = clampi(y, 0, (int32_t)scr_h - 1);
    return 0;
}

static int mouse_api_get_device_info(int idx, mouse_info_t *out)
{
    if (idx < 0 || idx >= MAX_POINT_DEVICES || !out) return -1;
    if (!mouse_devices[idx].used) return -1;
    memset(out->name, 0, sizeof(out->name));
    strncpy(out->name, mouse_devices[idx].name, sizeof(out->name) - 1);
    out->type = mouse_devices[idx].type;
    out->used = 1;
    out->buttons = 3;                  //пока все провайдеры — 3-кнопочные
    return 0;
}

static int mouse_api_register_device(const char *name, uint8_t type, uint8_t priority,
                                     int (*poll_fn)(void *priv, mouse_delta_t *out),
                                     void *priv)
{
    int i;
    if (!name || !poll_fn) return -1;

    for (i = 0; i < MAX_POINT_DEVICES; i++)
    {
        if (!mouse_devices[i].used)
        {
            memset(mouse_devices[i].name, 0, sizeof(mouse_devices[i].name));
            strncpy(mouse_devices[i].name, name, sizeof(mouse_devices[i].name) - 1);
            mouse_devices[i].type = type;
            mouse_devices[i].priority = priority;
            mouse_devices[i].poll_fn = poll_fn;
            mouse_devices[i].priv = priv;
            mouse_devices[i].used = 1;
            mouse_dev_count++;
            return i;                  //индекс в реестре
        }
    }
    return -1;                         //реестр полон
}

static int mouse_api_unregister_device(int idx)
{
    if (idx < 0 || idx >= MAX_POINT_DEVICES) return -1;
    if (!mouse_devices[idx].used) return -1;
    memset(&mouse_devices[idx], 0, sizeof(mouse_dev_entry_t));
    if (mouse_dev_count) mouse_dev_count--;
    return 0;
}

//Дескриптор драйвера

mouse_driver_api_t api;
mouse_driver_api_t *api_ptr;

description_driver_api_t descr_api;

void driver_main(struct boot_info *boot)
{
    int idx;

    //Границы экрана для клампа курсора
    if (boot)
    {
        scr_w = boot->width ? boot->width : 640;
        scr_h = boot->height ? boot->height : 480;
    }
    else
    {
        scr_w = 640;
        scr_h = 480;
    }

    memset(mouse_devices, 0, sizeof(mouse_devices));
    mouse_dev_count = 0;
    cur_x = (int32_t)scr_w / 2;
    cur_y = (int32_t)scr_h / 2;
    cur_buttons = 0;

    memset(&api, 0, sizeof(api));
    api.version = MOUSE_ABI_VERSION;
    api.poll = mouse_api_poll;
    api.set_pos = mouse_api_set_pos;
    api.get_device_info = mouse_api_get_device_info;
    api.register_device = mouse_api_register_device;
    api.unregister_device = mouse_api_unregister_device;

    //Дефолтный провайдер PS/2. Детект poll-овый: не зависит ни от PIC,
    //ни от isr.c, ни от irq_fired_ptr (его ядро пришивает после загрузки)
    idx = -1;
    if (mouse_ps2_init())
    {
        idx = mouse_api_register_device("ps2", PTYPE_PS2, 10, mouse_ps2_poll, 0);
    }

    api.enabled = (idx >= 0) ? 1 : 0;
    api.debug1 = mouse_dev_count;      //сколько устройств в реестре
    api.debug2 = dbg_packets;
    api.debug3 = dbg_resync;
    api.debug4 = init_step;            //шаг детекта при неудаче (MPST_*), 0 — все ок
    api.debug5 = (init_last_b < 0) ? 0xFFFFFFFFu : (uint32_t)init_last_b;

    api_ptr = &api;

    descr_api.type = DESCR_MOUSE_DRIVER;
    descr_api.struct_ptr = (void *)&api;
}

description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}
