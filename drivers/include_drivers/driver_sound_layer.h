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

#ifndef KERNEL_SOUND_LAYER_H
#define KERNEL_SOUND_LAYER_H

/*
 * kernel_sound_layer.h — высокоуровневый слой абстракции звука.
 *
 * Модель — упрощённая ALSA: snd_card -> snd_pcm_ops + snd_mixer_ops.
 * Такая форма выбрана специально под два будущих драйвера:
 *   - AC'97 Intel ICHx (Panasonic CF-18, ссылка intel8x0.c из Linux):
 *     кольцевой буфер с BDL, форматы S16_LE, 44100/48000, стерео;
 *   - Sound Blaster 16 (плата Geode): DSP + 8/16-бит DMA, те же структуры.
 *
 * Поток звука описывается параметрами snd_pcm_params_t (аналог
 * hw_params в ALSA): частота, каналы, формат, период и размер буфера.
 * Драйвер может работать как через DMA-кольцо (реализует pointer()),
 * так и через PIO (реализует transfer()).
 *
 * Заголовок общий для ядра и драйверов, чтобы описания структур совпадали
 * у обеих сторон. Нижний драйвер заполняет snd_driver_t и отдаёт его ядру
 * через description_driver_api_t (type = DESCR_SOUND_DRIVER, см.
 * drv_format.h). Регистрацией и выбором активного драйвера ведает
 * sound_layer.c (компилируется в ядро).
 *
 * Функции snd_* объявлены здесь для общности, но драйверы их не линкуют:
 * они вызываются только из кода ядра.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

//---------------- ABI ----------------
#define SND_LAYER_ABI_VERSION 1

//---------------- коды ошибок ----------------
#define SND_OK               0
#define SND_ERR_GENERIC    (-1)
#define SND_ERR_NODEV      (-2)   //нет активного драйвера
#define SND_ERR_UNSUPPORTED (-3)
#define SND_ERR_BADARG     (-4)
#define SND_ERR_BUSY       (-5)
#define SND_ERR_NOMEM      (-6)
#define SND_ERR_IO         (-7)
#define SND_ERR_BADFORMAT  (-8)   //формат/частота не поддерживается
#define SND_ERR_NOTOPEN    (-9)   //поток не открыт
#define SND_ERR_NOTFOUND   (-10)  //драйвер с таким именем не найден

//---------------- потоки ----------------
#define SND_STREAM_PLAYBACK 0
#define SND_STREAM_CAPTURE  1
#define SND_STREAM_COUNT    2

//---------------- команды trigger ----------------
#define SND_TRIGGER_START   1
#define SND_TRIGGER_STOP    2
#define SND_TRIGGER_PAUSE   3
#define SND_TRIGGER_RESUME  4

//---------------- форматы выборок (маска SND_FMT_*) ----------------
#define SND_FMT_U8     0x0001
#define SND_FMT_S8     0x0002
#define SND_FMT_S16_LE 0x0010
#define SND_FMT_U16_LE 0x0020
#define SND_FMT_S16_BE 0x0040
#define SND_FMT_U16_BE 0x0080
#define SND_FMT_S24_LE 0x0100
#define SND_FMT_S32_LE 0x0200

//---------------- частоты (маска SND_RATE_*) ----------------
#define SND_RATE_5512   0x00001
#define SND_RATE_8000   0x00002
#define SND_RATE_11025  0x00004
#define SND_RATE_16000  0x00008
#define SND_RATE_22050  0x00010
#define SND_RATE_32000  0x00020
#define SND_RATE_44100  0x00040
#define SND_RATE_48000  0x00080
#define SND_RATE_64000  0x00100
#define SND_RATE_88200  0x00200
#define SND_RATE_96000  0x00400
#define SND_RATE_176400 0x00800
#define SND_RATE_192000 0x01000
#define SND_RATE_CONT   0x02000   //непрерывный диапазон rate_min..rate_max

//---------------- возможности (snd_pcm_hw_info_t.features и snd_driver_t.flags) ----------------
#define SND_FEAT_DMA_RING    0x01  //кольцевой буфер + pointer()
#define SND_FEAT_PIO         0x02  //передача через transfer()
#define SND_FEAT_PLAYBACK    0x04
#define SND_FEAT_CAPTURE     0x08
#define SND_FEAT_FULL_DUPLEX 0x10
#define SND_FEAT_MIXER       0x20

//---------------- каналы микшера (маска SND_MIX_CH_*) ----------------
#define SND_MIX_CH_MASTER  0x01
#define SND_MIX_CH_PCM     0x02
#define SND_MIX_CH_CD      0x04
#define SND_MIX_CH_MIC     0x08
#define SND_MIX_CH_LINE    0x10
#define SND_MIX_CH_AUX     0x20
#define SND_MIX_CH_SPKR    0x40
#define SND_MIX_CH_REC     0x80  //общий уровень записи

//---------------- ограничения аппаратуры (аналог snd_pcm_hardware) ---
typedef struct snd_pcm_hw_info
{
    uint32_t formats;       //маска SND_FMT_*
    uint32_t rates_mask;    //маска SND_RATE_*; 0 = CONT по rate_min..rate_max
    uint32_t rate_min, rate_max;      //Гц
    uint32_t channels_min, channels_max;
    uint32_t period_min, period_max;  //в кадрах; 0 = не проверять
    uint32_t buffer_min, buffer_max;  //в кадрах; 0 = не проверять
    uint32_t features;      //SND_FEAT_*
} snd_pcm_hw_info_t;

//---------------- параметры потока (аналог hw_params) ----------------
typedef struct snd_pcm_params
{
    uint32_t rate;          // Гц
    uint32_t channels;      // 1 моно, 2 стерео, ...
    uint32_t format;        // один SND_FMT_* (значение, не маска)
    uint32_t period_size;   // кадров на период (прерывание драйвера)
    uint32_t buffer_size;   // кадров в кольцевом буфере
} snd_pcm_params_t;

//---------------- операции PCM (аналог snd_pcm_ops) ------------------
//Первый аргумент — priv из snd_driver_t. stream = SND_STREAM_*
typedef struct snd_pcm_ops
{
    uint32_t version;   //= SND_LAYER_ABI_VERSION

    const snd_pcm_hw_info_t *(*get_hw_info)(void *priv, uint32_t stream);

    int (*open)(void *priv, uint32_t stream);
    int (*close)(void *priv, uint32_t stream);

    //валидация и фиксация формата (после open, до prepare)
    int (*hw_params)(void *priv, uint32_t stream, const snd_pcm_params_t *params);

    //подготовка DMA/PIO к запуску: сброс указателей, заполнение BDL и т.п.
    int (*prepare)(void *priv, uint32_t stream);

    //SND_TRIGGER_*: старт/стоп/пауза/возобновление потока
    int (*trigger)(void *priv, uint32_t stream, int cmd);

    //текущая позиция в кольцевом буфере, в БАЙТАХ; 0xFFFFFFFF = неизвестна
    uint32_t (*pointer)(void *priv, uint32_t stream);

    //PIO-драйверам без DMA: залить данные напрямую в устройство
    int (*transfer)(void *priv, uint32_t stream, const void *buf, uint32_t bytes);
} snd_pcm_ops_t;

//---------------- микшер (аналог OSS-микшера, 0..100 на канал) -------
typedef struct snd_mixer_ops
{
    uint32_t version;   //= SND_LAYER_ABI_VERSION

    uint32_t (*get_channels)(void *priv);   //маска SND_MIX_CH_*
    int (*set_volume)(void *priv, uint32_t channel, uint8_t left, uint8_t right);
    int (*get_volume)(void *priv, uint32_t channel, uint8_t *left, uint8_t *right);
    int (*set_mute)(void *priv, uint32_t channel, int mute);
    int (*get_mute)(void *priv, uint32_t channel, int *mute);
    int (*set_rec_source)(void *priv, uint32_t channel);
} snd_mixer_ops_t;

//---------------- дескриптор драйвера (аналог snd_card) --------------
typedef struct snd_driver
{
    uint32_t version;       //= SND_LAYER_ABI_VERSION
    char     name[24];      //"sb16", "ac97_ich", "pcspk"
    char     longname[48];  //"Sound Blaster 16 @220h IRQ5 DMA1/5"
    uint32_t flags;         //SND_FEAT_*
    uint32_t priority;      //чем больше, тем вероятнее станет активным

    const snd_pcm_ops_t   *pcm;    //NULL = только микшер
    const snd_mixer_ops_t *mixer;  //NULL = микшера нет
    void *priv;

    //--- внедряемый ядром рантайм (заполняет sound_layer.c при
    //регистрации; драйвер может ничего тут не писать) ---
    void *(*malloc_ptr)(uint32_t size);
    void  (*free_ptr)(void *ptr);
    void  (*yield_ptr)(void);
    uint32_t (*get_ticks_ptr)(void);
    //физически непрерывная uncached-память под DMA-кольца (то же, что
    //использует USB-стек: pmm + vmm_map_pages_uncached + wbinvd)
    void *(*dma_alloc_ptr)(uint32_t size, uint32_t *phys_out);
    void  (*dma_free_ptr)(void *virt, uint32_t size);
    void  (*printf_ptr)(const char *fmt, ...);   //kprintf ядра
} snd_driver_t;


#endif /* KERNEL_SOUND_LAYER_H */
