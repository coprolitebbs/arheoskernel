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

//mouse_driver.c — системный драйвер указательных устройств (mouse.drv)
//Архитектура (по образцу block_driver.c):
//   - Реестр устройств mouse_devices[MAX_POINT_DEVICES] с poll_fn/priv
//   - Дефолтный провайдер PS/2 AUX (i8042, канал 1) встроен в драйвер и регистрируется в
//     driver_main() — это дефолтная функция получения координат
//   - Внешние провайдеры (USB HID, когда появится в usb-драйвере) входят через
//     mouse_register_device() — ровно так, как usb_msc входит в block_register_device()
//     После unregister USB-мыши активным снова становится PS/2
//   - Windows-модель: все устройства суммируются в дельты единого курсора, кнопки — OR по
//     всем устройствам. Абсолютные координаты хранит ядро (mouse_core.c), драйвер
//     отдаёт только дельты в экранных координатах
//
//Для PS/2:
//   - irq_callback() вызывается ядром из обработчика IRQ12 (если патч isr.c применён):
//     драйвер сам вычитывает байты из 0x60/0x64. Только порты, без ожиданий и без yield
//   - poll_fn() никогда не блокируется: перед выдачей дельт делает fallback-вычитку
//     AUX-буфера 8042. Драйвер полностью работает сам по себе (поллинг из mouse_core_tick
//     по тику таймера), но, можно попробовать перевести на получение статуса из прерывания в
//     isr_handler в isr.c. Будет медленнее работать.
//   - Взаимоблокировки с клавиатурой нет: клавиатурный байт (статус без бита AUX) мы не
//     вычитываем — его забирает kbd-драйвер из ISR1

#include "../include_drivers/drv_format.h"
#include "../../include-kernel/ports_io.h"

//===================== i8042 (PS/2 контроллер) =====================

#define PS2_DATA        0x60
#define PS2_STATUS      0x64
#define PS2_COMMAND     0x64

#define PS2_STS_OBF     0x01    //выходной буфер полон (можно читать 0x60)
#define PS2_STS_AUX     0x20    //последний байт в OBF — от мыши (канал 1)

#define PS2_CMD_ENABLE_AUX  0xA8
#define PS2_CMD_TEST_AUX    0xA9
#define PS2_CMD_READ_CCB    0x20
#define PS2_CMD_WRITE_CCB   0x60
#define PS2_CMD_WRITE_MOUSE 0xD4    //следующий байт в 0x60 уходит мыши

#define CCB_ENABLE_IRQ12    0x02    //разрешить прерывание канала 1
#define CCB_ENABLE_AUX_CLK  0xDF    //бит 5 = запрет AUX clock, снимаем его

//===================== Протокол мыши PS/2 =====================

#define MOUSE_ACK           0xFA
#define MOUSE_SELFTEST_OK   0xAA
#define MOUSE_SELFTEST_ERR  0xFC

#define M_CMD_RESET         0xFF
#define M_CMD_DEFAULTS      0xF6
#define M_CMD_IDENTIFY      0xF2
#define M_CMD_DISABLE       0xF5
#define M_CMD_ENABLE        0xF4
#define M_CMD_SAMPLE        0xF3

//Биты байта 0 пакета
#define PKT_YOVERF          0x80
#define PKT_XOVERF          0x40
#define PKT_ALWAYS1         0x08    //сегда 1 — используется для ресинхронизации

#define PS2_PKT_SIZE_BASE   3       //базовый пакет без колеса
#define PS2_PKT_SIZE_WHEEL  4       //пакет с колесом (после magic-последовательности)

#define MOUSE_IO_TIMEOUT    200000u //итераций busy-wait на байт
#define MOUSE_ACK_TRIES     6       //попыток вычитать ACK

//===================== Реестр устройств =====================

static mouse_dev_entry_t mouse_devices[MAX_POINT_DEVICES];
static int mouse_dev_count = 0;         //занятых слотов (включая снятые)
static int mouse_dev_live  = 0;         //активных (used) устройств

//===================== Состояние PS/2-провайдера =====================

typedef struct {
    uint8_t  present;       //мышь обнаружена при инициализации
    uint8_t  has_wheel;     //пакет 4 байта
    uint8_t  pkt[4];        //собираемый пакет
    uint8_t  pkt_idx;       //сколько байт уже собрано
    uint8_t  pkt_len;       //ожидаемая длина пакета (3 или 4)
    //Накопитель дельт с момента прошлого poll (в экранных координатах)
    volatile int16_t pend_dx;
    volatile int16_t pend_dy;
    volatile int16_t pend_wheel;
    volatile uint8_t pend_buttons;
    volatile uint8_t irq_mode;  //1 — как только ядро впервые позвало irq_callback
} ps2_mouse_state_t;

static ps2_mouse_state_t ps2;
static mouse_driver_api_t api;
static description_driver_api_t descr_api;

//===================== Служебные =====================

static void md_memset(void *dst, uint8_t val, uint32_t n){
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = val;
}

static int md_strcpy(char *dst, const char *src, int cap){
    int i = 0;
    while (src[i] && i < cap - 1){ dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return i;
}

static void md_maybe_yield(uint32_t counter){
    //Раз в ~50k итераций отдаём квант, если драйвер работает в контексте
    //задачи (driver_main), а не в прерывании.
    //В ISR yield не зовётся:
    //irq_callback/poll_fn не используют wait-функции с yield
    if (api.yield_ptr && (counter & 0x7FFF) == 0) api.yield_ptr();
}

//Ждать возможности записи в контроллер. 0 — ок, -1 — таймаут
static int ps2_wait_write(void){
    uint32_t t = MOUSE_IO_TIMEOUT;
    while (t--){
        if (!(inb(PS2_STATUS) & 0x02)) return 0;
        md_maybe_yield(t);
    }
    return -1;
}

//Ждать появления байта в выходном буфере. 0 — ок, -1 — таймаут
static int ps2_wait_read(void){
    uint32_t t = MOUSE_IO_TIMEOUT;
    while (t--){
        if (inb(PS2_STATUS) & PS2_STS_OBF) return 0;
        md_maybe_yield(t);
    }
    return -1;
}

//Отправить байт мыши (через 0xD4) и дождаться ACK
//Не годится для ISR — используется только при инициализации
static int mouse_send_byte(uint8_t b){
    if (ps2_wait_write()) return -1;
    outb(PS2_COMMAND, PS2_CMD_WRITE_MOUSE);
    if (ps2_wait_write()) return -1;
    outb(PS2_DATA, b);

    int tries = MOUSE_ACK_TRIES;
    while (tries--){
        if (ps2_wait_read()) return -2;
        uint8_t r = inb(PS2_DATA);
        if (r == MOUSE_ACK) return 0;
        //не ACK (поздний пакет/мусор) — выкинули, ждём дальше
    }
    return -3;
}

//Прочитать один байт от мыши с коротким таймаутом (после ACK)
//Возвращает байт через *out, 0 — ок, -1 — таймаут
static int mouse_read_byte(uint8_t *out){
    if (ps2_wait_read()) return -1;
    uint8_t b = inb(PS2_DATA);
    if (out) *out = b;
    return 0;
}

//===================== Поток данных PS/2 =====================

//Фид одного байта в декодер пакета. Вызывается из irq_callback и из fallback-поллинга
//Ресинхронизация: бит 3 байта 0 всегда 1
static void ps2_feed_byte(uint8_t b){
    if (ps2.pkt_idx == 0 && !(b & PKT_ALWAYS1)){
        //потеря синхронизации — выкидываем байт, счётчик и так 0
        return;
    }
    ps2.pkt[ps2.pkt_idx++] = b;
    if (ps2.pkt_idx < ps2.pkt_len) return;

    //Пакет собран
    ps2.pkt_idx = 0;
    uint8_t b0 = ps2.pkt[0];

    if (b0 & (PKT_XOVERF | PKT_YOVERF)){
        //переполнение счётчиков мыши — дельты ненадёжны, кнопки валидны
        ps2.pend_buttons = (uint8_t)(b0 & 0x07);
        return;
    }

    int8_t dx = (int8_t)ps2.pkt[1];
    int8_t dy = (int8_t)ps2.pkt[2];

    //PS/2: движение вниз = dy < 0. Экранные координаты: вниз = +
    int16_t sdx = (int16_t)dx;
    int16_t sdy = (int16_t)(-dy);

    ps2.pend_dx = (int16_t)(ps2.pend_dx + sdx);
    ps2.pend_dy = (int16_t)(ps2.pend_dy + sdy);
    ps2.pend_buttons = (uint8_t)(b0 & 0x07);

    if (ps2.has_wheel){
        int8_t wz = (int8_t)ps2.pkt[3];
        if (wz != 0) ps2.pend_wheel = (int16_t)(ps2.pend_wheel + (wz > 0 ? -1 : 1));
        //колесо вверх (wz > 0) уменьшает счётчик — как dy
    }
}

//Вычитать все доступные AUX-байты из буфера 8042
//Функция неблокирующая, безопасна из тика таймера и из ISR12
//Клавиатурные байты (статус без AUX-бита) не трогаем
static void ps2_drain(void){
    uint32_t guard = 64;    //не больше 64 байт за раз
    while (guard--){
        uint8_t st = inb(PS2_STATUS);
        if (!(st & PS2_STS_OBF)) return;    //буфер пуст
        if (!(st & PS2_STS_AUX)){
            //Это байт клавиатуры — его вычитает kbd-драйвер из ISR1
            //Выходим: наш байт придёт следующим, заберём потом
            return;
        }
        ps2_feed_byte(inb(PS2_DATA));
    }
}

//===================== PS/2-провайдер: API реестра =====================

static int ps2_mouse_poll(void *priv, mouse_delta_t *out){
    (void)priv;
    if (!out) return -1;
    ps2_drain();    //fallback-вычитка: работает и без IRQ12
    out->dx      = ps2.pend_dx;
    out->dy      = ps2.pend_dy;
    out->wheel   = ps2.pend_wheel;
    out->buttons = ps2.pend_buttons;
    ps2.pend_dx = 0; ps2.pend_dy = 0; ps2.pend_wheel = 0;
    return ps2.present ? 0 : -2;
}

static int ps2_mouse_caps(void *priv, mouse_caps_t *out){
    (void)priv;
    if (!out) return -1;
    out->buttons   = 3;
    out->has_wheel = ps2.has_wheel;
    return 0;
}

//===================== Реестр устройств =====================

static int mouse_register_device_impl(const char *name, uint8_t type,
                                      int (*poll_fn)(void*, mouse_delta_t*),
                                      int (*get_caps_fn)(void*, mouse_caps_t*),
                                      void *priv){
    if (!name || !poll_fn) return FS_IO_ERROR;
    if (mouse_dev_count >= MAX_POINT_DEVICES) return FS_TOO_MANY;

    int idx = -1;
    for (int i = 0; i < MAX_POINT_DEVICES; i++){
        if (!mouse_devices[i].used){ idx = i; break; }
    }
    if (idx < 0) return FS_TOO_MANY;

    mouse_dev_entry_t *e = &mouse_devices[idx];
    md_memset(e, 0, sizeof(*e));
    md_strcpy(e->name, name, (int)sizeof(e->name));
    e->type        = type;
    e->used        = 1;
    e->poll_fn     = poll_fn;
    e->get_caps_fn = get_caps_fn;
    e->priv        = priv;

    mouse_dev_count++;
    mouse_dev_live++;
    api.enabled = (mouse_dev_live > 0);
    return idx;
}

static int mouse_unregister_device_impl(int idx){
    if (idx < 0 || idx >= MAX_POINT_DEVICES) return FS_IO_ERROR;
    if (!mouse_devices[idx].used) return FS_NOT_FOUND;
    mouse_devices[idx].used = 0;
    mouse_dev_live--;
    api.enabled = (mouse_dev_live > 0);
    return FS_OK;
}

static int mouse_get_device_count_impl(void){
    return mouse_dev_live;
}

static int mouse_get_device_info_impl(int idx, mouse_info_t *out){
    if (!out) return FS_IO_ERROR;
    if (idx < 0 || idx >= MAX_POINT_DEVICES) return FS_NOT_FOUND;
    if (!mouse_devices[idx].used) return FS_NOT_FOUND;

    md_memset(out, 0, sizeof(*out));
    md_strcpy(out->name, mouse_devices[idx].name, (int)sizeof(out->name));
    out->type = mouse_devices[idx].type;
    out->used = 1;
    out->has_wheel = 0;
    if (mouse_devices[idx].get_caps_fn){
        mouse_caps_t caps;
        if (mouse_devices[idx].get_caps_fn(mouse_devices[idx].priv, &caps) == 0)
            out->has_wheel = caps.has_wheel;
    }
    return FS_OK;
}

//===================== Высокоуровневый poll (Windows-модель) =====================

//Сумма дельт всех активных устройств; кнопки — OR по всем
//int32-аккумулятор на случай, если провайдеров несколько и каждый накопил по int16 за период опроса
static int mouse_poll_impl(mouse_delta_t *out){
    if (!out) return FS_IO_ERROR;

    int32_t adx = 0, ady = 0, awheel = 0;
    uint8_t abtn = 0;
    int got = 0;

    for (int i = 0; i < MAX_POINT_DEVICES; i++){
        mouse_dev_entry_t *e = &mouse_devices[i];
        if (!e->used || !e->poll_fn) continue;
        mouse_delta_t d;
        if (e->poll_fn(e->priv, &d) != 0) continue;
        got = 1;
        adx += d.dx; ady += d.dy; awheel += d.wheel;
        abtn |= d.buttons;
    }

    if (adx >  32767) adx =  32767;
    else if (adx < -32768) adx = -32768;
    if (ady >  32767) ady =  32767;
    else if (ady < -32768) ady = -32768;
    if (awheel > 32767) awheel =  32767;
    else if (awheel < -32768) awheel = -32768;

    out->dx      = (int16_t)adx;
    out->dy      = (int16_t)ady;
    out->wheel   = (int16_t)awheel;
    out->buttons = abtn;
    return got ? FS_OK : FS_NOT_FOUND;
}

//===================== IRQ12 (подключается при использовании прерывания из isr.c) =====================

static void mouse_irq_callback_impl(void){
    ps2.irq_mode = 1;
    if (api.irq_fired_ptr) *api.irq_fired_ptr = 1;
    ps2_drain();
}

//===================== Инициализация PS/2 AUX =====================

//Разрешить канал 1 в контроллере и CCB (IRQ12, AUX clock)
static int ps2_enable_aux_port(void){
    if (ps2_wait_write()) return -1;
    outb(PS2_COMMAND, PS2_CMD_ENABLE_AUX);

    if (ps2_wait_write()) return -1;
    outb(PS2_COMMAND, PS2_CMD_READ_CCB);
    if (ps2_wait_read()) return -1;
    uint8_t ccb = inb(PS2_DATA);

    ccb |= CCB_ENABLE_IRQ12;      //разрешить IRQ12
    ccb &= CCB_ENABLE_AUX_CLK;    //разрешить clock канала 1

    if (ps2_wait_write()) return -1;
    outb(PS2_COMMAND, PS2_CMD_WRITE_CCB);
    if (ps2_wait_write()) return -1;
    outb(PS2_DATA, ccb);
    return 0;
}

//Сброс мыши: 0xFF -> ACK, 0xAA, идентификатор
//0 — мышь есть, -1 — мыши нет / не отвечает
static int ps2_mouse_reset(uint8_t *out_id){
    if (mouse_send_byte(M_CMD_RESET)) return -1;

    uint8_t b = 0;
    if (mouse_read_byte(&b)) return -1;         //self-test результат
    if (b != MOUSE_SELFTEST_OK) return -1;      //0xFC/мусор — мышь битая

    //device ID (0x00 стандартная, 0x03 wheel-режим).
    //У некоторых мышей ID приходит с задержкой — отсутствие байта не признак отсутствия мыши,
    //поэтому таймаут не фейлит инициализацию
    if (mouse_read_byte(&b) == 0 && out_id) *out_id = b;
    return 0;
}

//Magic-последовательность включения колеса: sample 200,100,80 -> identify
//1 — мышь с колесом (ID 0x03)
static int ps2_detect_wheel(void){
    //Параметр команды 0xF3 уходит так же через 0xD4 и получает ACK,
    //поэтому каждый элемент — mouse_send_byte
    if (mouse_send_byte(M_CMD_SAMPLE)) return 0;
    if (mouse_send_byte(0xC8)) return 0;
    if (mouse_send_byte(M_CMD_SAMPLE)) return 0;
    if (mouse_send_byte(0x64)) return 0;
    if (mouse_send_byte(M_CMD_SAMPLE)) return 0;
    if (mouse_send_byte(0x50)) return 0;

    if (mouse_send_byte(M_CMD_IDENTIFY)) return 0;
    uint8_t id = 0;
    if (mouse_read_byte(&id)) return 0;
    return (id == 0x03) ? 1 : 0;
}

static int ps2_mouse_init(void){
    md_memset(&ps2, 0, sizeof(ps2));
    ps2.pkt_len = PS2_PKT_SIZE_BASE;

    if (ps2_enable_aux_port()) return -1;

    uint8_t id = 0;
    if (ps2_mouse_reset(&id)) return -1;

    //Конфигурация: defaults, детект колеса, включение reporting
    if (mouse_send_byte(M_CMD_DISABLE))  return -1;
    if (mouse_send_byte(M_CMD_DEFAULTS)) return -1;

    ps2.has_wheel = (uint8_t)ps2_detect_wheel();
    ps2.pkt_len = ps2.has_wheel ? PS2_PKT_SIZE_WHEEL : PS2_PKT_SIZE_BASE;

    if (mouse_send_byte(M_CMD_ENABLE)) return -1;

    ps2.present = 1;
    return 0;
}

//===================== Входная точка драйвера =====================

void driver_main(struct boot_info *boot){
    (void)boot;

    md_memset(&api, 0, sizeof(api));
    md_memset(mouse_devices, 0, sizeof(mouse_devices));
    mouse_dev_count = 0;
    mouse_dev_live  = 0;

    //PS/2 AUX — дефолтный провайдер
    //Регистрируем всегда, даже если мышь не ответила:
    //poll_fn вернёт -2, usb-провайдер сможет встать позже, ядро увидит честный enabled=0
    int r = ps2_mouse_init();
    int idx = mouse_register_device_impl("ps2", MOUSE_PTYPE_PS2,
                                         ps2_mouse_poll, ps2_mouse_caps, &ps2);
    if (idx < 0) r = -1;

    api.version = 1;
    api.enabled = (mouse_dev_live > 0) && (r == 0);
    api.poll              = mouse_poll_impl;
    api.get_device_count  = mouse_get_device_count_impl;
    api.get_device_info   = mouse_get_device_info_impl;
    api.register_device   = mouse_register_device_impl;
    api.unregister_device = mouse_unregister_device_impl;
    api.irq_callback      = mouse_irq_callback_impl;
    //malloc_ptr/free_ptr/yield_ptr/irq_fired_ptr пришьет ядро в use_drivers.c

    api.debug1 = (uint32_t)r;             //0 — PS/2 мышь обнаружена
    api.debug2 = (uint32_t)mouse_dev_live;
    api.debug1 |= ((uint32_t)ps2.has_wheel << 8);

    descr_api.type       = DESCR_MOUSE_DRIVER;
    descr_api.struct_ptr = (void *)&api;
}

description_driver_api_t *driver_get_api(void){
    return &descr_api;
}
