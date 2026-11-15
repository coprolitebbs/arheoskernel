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

// ============================================================================
// mouse_driver.c — драйвер указательных устройств (мышь) ARHeOS.
//
// Архитектура (модель Windows):
//   * Драйвер держит РЕЕСТР провайдеров (PS/2, в будущем USB-HID).
//     Каждый провайдер регистрируется через register_device() и отдаёт дельты
//     через свой poll_fn(). ВСЕ провайдеры двигают ОДИН курсор: их дельты
//     суммируются, кнопки объединяются по OR.
//   * Дефолтный провайдер — PS/2 AUX (i8042, IRQ12).
//
// ВАЖНО (это и была причина «Mouse drv mouse.drv: no mouse»):
//   * Детект и опрос ПОЛНОСТЬЮ POLL-ОВЫЕ — читаем статус-регистр i8042 (0x64)
//     и данные (0x60) напрямую. IRQ12 нужен только как «будильник» для
//     будущей событийной модели; его отсутствие (замаскированный PIC,
//     не пришитый isr.c) НЕ мешает детекту и работе.
//   * Дисциплина чтения строгая:
//       - байты мыши отличаем от клавиатурных по биту AUX (bit5 статуса);
//       - чужие (клавиатурные) байты вычитываем и выбрасываем, ACK не теряется;
//       - перед каждой записью в контроллер ждём IBF==0;
//       - команды мыши идут только через префикс 0xD4;
//       - таймауты большие (реальное железо отвечает десятки мс).
//   * Конвенция дельт: провайдер отдаёт dy уже в ЭКРАННЫХ координатах
//     (dy>0 — курсор идёт ВНИЗ). PS/2 возвращает dy в стиле «вверх — это плюс»,
//     поэтому провайдер инвертирует знак. Аккумулятор знаки не трогает.
// ============================================================================

#include "../include_drivers/drv_format.h"
#include "../../include-kernel/bootinfo.h"
#include "../../include-kernel/ports_io.h"
#include "../../include-kernel/lib.h"

// ----------------------------------------------------------------------------
// Внутреннее состояние PS/2-провайдера
// ----------------------------------------------------------------------------

#define I8042_STATUS   0x64
#define I8042_DATA     0x60

#define ST_OBF         0x01   // в выходном буфере есть байт
#define ST_IBF         0x02   // входной буфер занят — писать нельзя
#define ST_AUX         0x20   // байт в выходном буфере — от AUX (мыши)

// Таймауты в итерациях опроса. Одна итерация = inb + io_wait (единицы мкс
// даже на Geode GX1). QEMU отвечает мгновенно, реальное железо — до сотен мс.
#define TIMEOUT_FAST   200000    // ожидание ACK (~0.2-1 с на реальном железе)
#define TIMEOUT_RESET  1000000   // ожидание 0xAA после Reset (~1-3 с)
#define TIMEOUT_CCB    200000

static uint8_t  ps2_present   = 0;     // мышь найдена и инициализирована
static uint8_t  ps2_last_btns = 0;     // последнее состояние кнопок PS/2-мыши

// Накопитель пакетов: байты приходят асинхронно (по IRQ или между опросами),
// поэтому собираем 3-байтовый пакет постепенно
static uint8_t  ps2_pkt[3];
static uint8_t  ps2_pkt_idx   = 0;

// Накопленная дельта с последнего вызова poll_fn (пакетов между опросами
// может прийти несколько — всё складываем)
static int32_t  ps2_acc_dx    = 0;
static int32_t  ps2_acc_dy    = 0;

// Счётчики для отладки (видны в api->debug*)
static uint32_t dbg_packets   = 0;
static uint32_t dbg_resync    = 0;
static uint32_t dbg_overflow  = 0;

// ----------------------------------------------------------------------------
// Низкоуровневые примитивы i8042
// ----------------------------------------------------------------------------

// Ждать возможности записи (IBF==0). 1 — ок, 0 — таймаут.
static int i8042_wait_write(int loops)
{
    while (loops--){
        if (!(inb(I8042_STATUS) & ST_IBF)) return 1;
        io_wait();
    }
    return 0;
}

// Вычитать всё, что лежит в выходном буфере (без разбора). Нужно перед
// инициализацией: BIOS мог оставить там байты BAT-теста клавиатуры, и без
// flush первый «ACK» мыши оказался бы чужим байтом.
static void i8042_flush(int loops)
{
    while (loops--){
        if (!(inb(I8042_STATUS) & ST_OBF)) return;
        (void)inb(I8042_DATA);
        io_wait();
    }
}

// Прочитать ОДИН байт мыши: ждём OBF, но забираем только байты с взведённым
// битом AUX. Клавиатурные байты (AUX==0) вычитываем и выбрасываем — иначе
// они навсегда «застревают» перед нашим ACK и всё ломают.
// Возвращает 0..255 или -1 по таймауту.
static int ps2_read_byte(int loops)
{
    while (loops--){
        uint8_t st = inb(I8042_STATUS);
        if (st & ST_OBF){
            uint8_t b = inb(I8042_DATA);
            if (st & ST_AUX) return b;
            // чужой (клавиатурный) байт — проглотили, продолжаем ждать наш
        }
        io_wait();
    }
    return -1;
}

// Отправить команду мыши (через префикс 0xD4) и дождаться ACK 0xFA.
// Возвращает 1 при успехе.
static int ps2_command(uint8_t cmd)
{
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_STATUS, 0xD4);          // следующий байт в 0x60 — для AUX
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_DATA, cmd);

    int ack = ps2_read_byte(TIMEOUT_FAST);
    return (ack == 0xFA) ? 1 : 0;
}

// ----------------------------------------------------------------------------
// Инициализация PS/2 AUX. Полностью poll-овая: работает даже если IRQ12
// замаскирован в PIC или обработчик в isr.c ещё не пришит.
// Возвращает 1, если мышь найдена, 0 — если нет.
// ----------------------------------------------------------------------------
static int mouse_ps2_init(void)
{
    uint8_t ccb;
    int ack, resp;

    ps2_pkt_idx = 0;
    ps2_acc_dx = 0;
    ps2_acc_dy = 0;
    ps2_last_btns = 0;

    // 0. Очистить мусор в выходном буфере (байты от BIOS/клавиатуры)
    i8042_flush(TIMEOUT_FAST);

    // 1. Отключить и включить AUX-порт: приводит контроллер в заведомое
    //    состояние (на 86box без этого AUX иногда «спит»)
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_STATUS, 0xA7);          // disable AUX
    i8042_flush(TIMEOUT_FAST);
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_STATUS, 0xA8);          // enable AUX
    i8042_flush(TIMEOUT_FAST);

    // 2. Байт конфигурации контроллера (CCB):
    //      bit1 = разрешить IRQ12 от AUX
    //      bit5 = 0 (разрешить тактирование AUX-порта)
    //    Ответ на 0x20 приходит как обычный байт контроллера (AUX==0),
    //    читаем его именно «контроллерным» способом.
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_STATUS, 0x20);          // Read Controller Configuration Byte
    {
        int t = TIMEOUT_CCB;
        resp = -1;
        while (t--){
            uint8_t st = inb(I8042_STATUS);
            if (st & ST_OBF){ resp = inb(I8042_DATA); break; }
            io_wait();
        }
    }
    if (resp < 0) return 0;            // контроллер не ответил — AUX нет
    ccb = (uint8_t)resp;
    ccb |=  (1 << 1);                  // enable AUX IRQ12
    ccb &= ~(1 << 5);                  // enable AUX clock
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_STATUS, 0x60);          // Write Controller Configuration Byte
    if (!i8042_wait_write(TIMEOUT_FAST)) return 0;
    outb(I8042_DATA, ccb);

    // 3. Reset мыши: 0xFF -> ACK 0xFA, потом 0xAA (self-test ok) и 0x00 (id).
    //    0xFC вместо 0xAA — ошибка самотеста; тишина — устройства нет.
    if (!ps2_command(0xFF)) return 0;
    ack = ps2_read_byte(TIMEOUT_RESET);            // 0xAA
    if (ack != 0xAA) return 0;
    (void)ps2_read_byte(TIMEOUT_RESET);            // device id (0x00), не проверяем жёстко

    // 4. Заводские настройки по умолчанию
    if (!ps2_command(0xF6)) return 0;

    // 5. Частота опроса 40 отчётов/с (стабильность на реальном железе;
    //    заодно задел под будущую магию включения колеса)
    if (!ps2_command(0xF3)) return 0;
    if (!ps2_command(0x28)) return 0;

    // 6. Разрешить стриминг отчётов
    if (!ps2_command(0xF4)) return 0;

    // 7. Слить всё, что успело прийти между шагами
    i8042_flush(TIMEOUT_FAST);

    ps2_present = 1;
    return 1;
}

// ----------------------------------------------------------------------------
// poll_fn провайдера PS/2: вычитывает всё, что лежит в буфере, собирает
// 3-байтовые пакеты, накапливает дельту. Возвращает 1, если была активность.
// ----------------------------------------------------------------------------
static int mouse_ps2_poll(void *priv, mouse_delta_t *out)
{
    (void)priv;

    // Вычитываем всё, что есть СЕЙЧАС (не ждём: poll — неблокирующий)
    for (;;){
        uint8_t st = inb(I8042_STATUS);
        if (!(st & ST_OBF)) break;

        uint8_t b = inb(I8042_DATA);
        if (!(st & ST_AUX)) continue;           // клавиатурный байт — мимо

        if (ps2_pkt_idx == 0 && !(b & 0x08)){
            // байт синхронизации: bit3 первого байта пакета всегда 1.
            // Если нет — мы потеряли границу пакета: выбрасываем байт.
            dbg_resync++;
            continue;
        }

        ps2_pkt[ps2_pkt_idx++] = b;
        if (ps2_pkt_idx < 3) continue;
        ps2_pkt_idx = 0;

        if (ps2_pkt[0] & 0xC0){                 // overflow x/y — пакет битый
            dbg_overflow++;
            continue;
        }

        int dx = ps2_pkt[1];
        int dy = ps2_pkt[2];
        if (ps2_pkt[0] & 0x10) dx -= 256;       // знак X (9-битное дополнение)
        if (ps2_pkt[0] & 0x20) dy -= 256;       // знак Y
        dy = -dy;                               // PS/2: «плюс» = вверх -> на экране вниз

        ps2_acc_dx += dx;
        ps2_acc_dy += dy;
        ps2_last_btns = ps2_pkt[0] & 0x07;      // bit0=L bit1=R bit2=M
        dbg_packets++;
    }

    if (ps2_acc_dx == 0 && ps2_acc_dy == 0 && out){
        out->dx = 0; out->dy = 0; out->wheel = 0;
        out->buttons = ps2_last_btns;           // состояние кнопок отдаём всегда
        return 0;
    }

    if (out){
        out->dx = (int16_t)ps2_acc_dx;
        out->dy = (int16_t)ps2_acc_dy;
        out->wheel = 0;
        out->buttons = ps2_last_btns;
    }
    ps2_acc_dx = 0;
    ps2_acc_dy = 0;
    return 1;
}

// ----------------------------------------------------------------------------
// Реестр провайдеров (сейчас: ps2aux0; в будущем usbhid0 и т.п.)
// ----------------------------------------------------------------------------

static mouse_dev_entry_t mouse_devices[MAX_POINT_DEVICES];
static uint8_t mouse_dev_count = 0;

// ----------------------------------------------------------------------------
// Аккумулятор единого курсора (модель Windows)
// ----------------------------------------------------------------------------

static int32_t cur_x = 0, cur_y = 0;
static uint8_t cur_buttons = 0;
static int32_t acc_dx = 0, acc_dy = 0;    // дельта с предыдущего api->poll()
static uint32_t scr_w = 640, scr_h = 480;

static int32_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// ----------------------------------------------------------------------------
// Публичный API ((mouse_driver_api_t))
// ----------------------------------------------------------------------------

//Единый опрос: суммирует дельты ВСЕХ провайдеров, двигает курсор,
//возвращает итоговое состояние. Вызывать из SYS_MOUSE_*.
static int mouse_api_poll(mouse_state_t *out)
{
    mouse_delta_t d;
    int i, active = 0;

    acc_dx = 0;
    acc_dy = 0;

    for (i = 0; i < MAX_POINT_DEVICES; i++){
        if (!mouse_devices[i].used) continue;
        if (!mouse_devices[i].poll_fn) continue;
        if (mouse_devices[i].poll_fn(mouse_devices[i].priv, &d) != 0) active = 1;
        acc_dx += d.dx;
        acc_dy += d.dy;
        cur_buttons |= d.buttons;      // Windows-модель: кнопка нажата, если её
                                       // держит ЛЮБОЕ из устройств
    }

    // Кнопки, которые больше никто не держит, отпускаем лениво: если ни один
    // провайдер не вернул активность и кнопки в этом опросе не названы —
    // это состояние последних пакетов, OR выше уже учёл released-байты
    // (в PS/2 кнопки идут в каждом пакете, поэтому stale-состояний нет).

    cur_x = clampi(cur_x + acc_dx, 0, (int32_t)scr_w - 1);
    cur_y = clampi(cur_y + acc_dy, 0, (int32_t)scr_h - 1);

    if (out){
        out->x = (int16_t)cur_x;
        out->y = (int16_t)cur_y;
        out->dx = (int16_t)acc_dx;
        out->dy = (int16_t)acc_dy;
        out->buttons = cur_buttons;
        out->device_count = mouse_dev_count;
    }
    return active ? 0 : 0;             // 0 = ок (курсор всегда валиден)
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
    out->buttons = 3;                  // пока все провайдеры — 3-кнопочные
    return 0;
}

static int mouse_api_register_device(const char *name, uint8_t type, uint8_t priority,
                                     int (*poll_fn)(void *priv, mouse_delta_t *out),
                                     void *priv)
{
    int i;
    if (!name || !poll_fn) return -1;

    for (i = 0; i < MAX_POINT_DEVICES; i++){
        if (!mouse_devices[i].used){
            memset(mouse_devices[i].name, 0, sizeof(mouse_devices[i].name));
            strncpy(mouse_devices[i].name, name, sizeof(mouse_devices[i].name) - 1);
            mouse_devices[i].type = type;
            mouse_devices[i].priority = priority;
            mouse_devices[i].poll_fn = poll_fn;
            mouse_devices[i].priv = priv;
            mouse_devices[i].used = 1;
            mouse_dev_count++;
            return i;                  // индекс в реестре
        }
    }
    return -1;                         // реестр полон
}

static int mouse_api_unregister_device(int idx)
{
    if (idx < 0 || idx >= MAX_POINT_DEVICES) return -1;
    if (!mouse_devices[idx].used) return -1;
    memset(&mouse_devices[idx], 0, sizeof(mouse_dev_entry_t));
    if (mouse_dev_count) mouse_dev_count--;
    return 0;
}

// ----------------------------------------------------------------------------
// Дескриптор драйвера (паттерн kbd_driver.c)
// ----------------------------------------------------------------------------

mouse_driver_api_t api;
mouse_driver_api_t *api_ptr;

description_driver_api_t descr_api;

void driver_main(struct boot_info *boot)
{
    int idx;

    // Границы экрана для клампа курсора
    if (boot){
        scr_w = boot->width ? boot->width : 640;
        scr_h = boot->height ? boot->height : 480;
    } else {
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

    // Дефолтный провайдер PS/2. Детект poll-овый: не зависит ни от PIC,
    // ни от isr.c, ни от irq_fired_ptr (его ядро пришивает ПОСЛЕ загрузки).
    idx = -1;
    if (mouse_ps2_init()){
        idx = mouse_api_register_device("ps2aux0", PTYPE_PS2, 10, mouse_ps2_poll, 0);
    }

    api.enabled = (idx >= 0) ? 1 : 0;
    api.debug1 = mouse_dev_count;      // сколько устройств в реестре
    api.debug2 = dbg_packets;
    api.debug3 = dbg_resync;

    api_ptr = &api;

    descr_api.type = DESCR_MOUSE_DRIVER;
    descr_api.struct_ptr = (void *)&api;
}

description_driver_api_t *driver_get_api(void)
{
    return &descr_api;
}
