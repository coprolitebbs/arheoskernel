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

/*
 * sound_layer.c — реализация высокоуровневого слоя звука (Ring 0).
 * API описан в drivers/include_drivers/kernel_sound_layer.h.
 *
 * Здесь только реестр snd_driver_t с автопереключением по priority,
 * кэш параметров потоков и проходные snd_*-функции. Fallback-драйвера
 * (как fbdrv у графики) нет: пока ни один звуковой драйвер не загружен,
 * все snd_*-функции честно возвращают SND_ERR_NODEV.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "drivers/include_drivers/driver_sound_layer.h"
#include "include-kernel/sound_layer.h"
#include "drivers/include_drivers/drv_format.h"
#include "include-kernel/kernel_heap.h"
#include "include-kernel/isr.h"
#include "include-kernel/lib.h"
#include "include-kernel/kconsole.h"
#include "include-kernel/use_drivers.h"

//---------------- реестр драйверов ----------------
#define SND_DRIVERS_MAX 8

static snd_driver_t *snd_registry[SND_DRIVERS_MAX];
static uint32_t snd_driver_count = 0;
static snd_driver_t *snd_active = 0;
static int snd_initialized = 0;

//Кэш параметров открытых потоков (заполняется snd_pcm_set_params)
static snd_pcm_params_t snd_cur_params[SND_STREAM_COUNT];
static uint8_t snd_stream_open[SND_STREAM_COUNT];

//Тики ядра (PIT 100 Гц) для драйверов — как kernel_get_ticks в usb-api
static uint32_t snd_layer_get_ticks(void)
{
    return tick_count;
}

/* ---------------- внедрение ядерного рантайма ---------------- */

static void snd_inject_runtime(snd_driver_t *drv)
{
    if (!drv->malloc_ptr)    drv->malloc_ptr    = kmalloc;
    if (!drv->free_ptr)      drv->free_ptr      = kfree;
    if (!drv->yield_ptr)     drv->yield_ptr     = sys_yield;
    if (!drv->get_ticks_ptr) drv->get_ticks_ptr = snd_layer_get_ticks;
    /* DMA-память: тот же путь, что и у USB-стека (uncached + wbinvd) */
    if (!drv->dma_alloc_ptr) drv->dma_alloc_ptr = kernel_usb_dma_alloc;
    if (!drv->dma_free_ptr)  drv->dma_free_ptr  = kernel_usb_dma_free;
    if (!drv->printf_ptr)    drv->printf_ptr    = kprintf;
}

/* ---------------- реестр драйверов ---------------- */

int sound_layer_register_driver(snd_driver_t *drv)
{
    if (!drv || (!drv->pcm && !drv->mixer)) return SND_ERR_BADARG;
    if (drv->version != SND_LAYER_ABI_VERSION)
    {
        if (drv->printf_ptr)
            drv->printf_ptr("SND: driver '%s' rejected, ABI %d != %d\n",
                            drv->name, (int)drv->version, SND_LAYER_ABI_VERSION);
        return SND_ERR_BADARG;
    }
    for (uint32_t i = 0; i < snd_driver_count; i++)
        if (snd_registry[i] == drv) return SND_ERR_BUSY;
    if (snd_driver_count >= SND_DRIVERS_MAX) return SND_ERR_NOMEM;

    snd_inject_runtime(drv);
    snd_registry[snd_driver_count++] = drv;

    /* автопереключение: нет активного или новый не менее приоритетный */
    if (!snd_active || drv->priority >= snd_active->priority)
        snd_active = drv;
    return SND_OK;
}

int sound_layer_unregister_driver(const char *name)
{
    for (uint32_t i = 0; i < snd_driver_count; i++)
    {
        if (strcmp(snd_registry[i]->name, name) == 0)
        {
            for (uint32_t j = i; j + 1 < snd_driver_count; j++)
                snd_registry[j] = snd_registry[j + 1];
            snd_driver_count--;
            /* пересчёт активного: старший по priority из оставшихся */
            snd_active = 0;
            for (uint32_t k = 0; k < snd_driver_count; k++)
                if (!snd_active || snd_registry[k]->priority > snd_active->priority)
                    snd_active = snd_registry[k];
            return SND_OK;
        }
    }
    return SND_ERR_NOTFOUND;
}

int sound_layer_activate_driver(const char *name)
{
    for (uint32_t i = 0; i < snd_driver_count; i++)
    {
        if (strcmp(snd_registry[i]->name, name) == 0)
        {
            snd_active = snd_registry[i];
            return SND_OK;
        }
    }
    return SND_ERR_NOTFOUND;
}

snd_driver_t *sound_layer_get_active(void) { return snd_active; }
uint32_t sound_layer_get_driver_count(void) { return snd_driver_count; }

/* ---------------- инициализация слоя ---------------- */

void init_sound_layer(void)
{
    if (snd_initialized) return;
    snd_initialized = 1;
    memset(snd_cur_params, 0, sizeof(snd_cur_params));
    memset(snd_stream_open, 0, sizeof(snd_stream_open));
    /* Звуковых fallback-драйверов в ядре нет; когда ни один драйвер не
     * зарегистрирован, snd_* функции возвращают SND_ERR_NODEV. */
}

/* ---------------- PCM ---------------- */

int snd_pcm_open(uint32_t stream)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->open)
        return SND_ERR_NODEV;
    int rc = snd_active->pcm->open(snd_active->priv, stream);
    if (rc == SND_OK)
    {
        snd_stream_open[stream] = 1;
        memset(&snd_cur_params[stream], 0, sizeof(snd_pcm_params_t));
    }
    return rc;
}

int snd_pcm_close(uint32_t stream)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->close)
        return SND_ERR_NODEV;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    int rc = snd_active->pcm->close(snd_active->priv, stream);
    snd_stream_open[stream] = 0;
    memset(&snd_cur_params[stream], 0, sizeof(snd_pcm_params_t));
    return rc;
}

int snd_pcm_get_hw_info(uint32_t stream, snd_pcm_hw_info_t *out)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!out) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm) return SND_ERR_NODEV;
    if (!snd_active->pcm->get_hw_info) return SND_ERR_UNSUPPORTED;
    const snd_pcm_hw_info_t *hi = snd_active->pcm->get_hw_info(snd_active->priv, stream);
    if (!hi) return SND_ERR_UNSUPPORTED;
    *out = *hi;
    return SND_OK;
}

/* частота -> бит SND_RATE_* (для дискретных списков) */
static uint32_t snd_rate_bit(uint32_t rate)
{
    switch (rate)
    {
    case 5512:   return SND_RATE_5512;
    case 8000:   return SND_RATE_8000;
    case 11025:  return SND_RATE_11025;
    case 16000:  return SND_RATE_16000;
    case 22050:  return SND_RATE_22050;
    case 32000:  return SND_RATE_32000;
    case 44100:  return SND_RATE_44100;
    case 48000:  return SND_RATE_48000;
    case 64000:  return SND_RATE_64000;
    case 88200:  return SND_RATE_88200;
    case 96000:  return SND_RATE_96000;
    case 176400: return SND_RATE_176400;
    case 192000: return SND_RATE_192000;
    default:     return 0;
    }
}

int snd_pcm_set_params(uint32_t stream, const snd_pcm_params_t *params)
{
    if (stream >= SND_STREAM_COUNT || !params) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm) return SND_ERR_NODEV;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    if (!snd_active->pcm->hw_params) return SND_ERR_UNSUPPORTED;

    /* предварительная валидация по hw_info, если драйвер её отдаёт */
    if (snd_active->pcm->get_hw_info)
    {
        const snd_pcm_hw_info_t *hi =
            snd_active->pcm->get_hw_info(snd_active->priv, stream);
        if (hi)
        {
            if (!(hi->formats & params->format)) return SND_ERR_BADFORMAT;
            if (params->channels < hi->channels_min ||
                params->channels > hi->channels_max) return SND_ERR_BADARG;
            if (hi->rates_mask & SND_RATE_CONT)
            {
                if (params->rate < hi->rate_min || params->rate > hi->rate_max)
                    return SND_ERR_BADFORMAT;
            }
            else if (hi->rates_mask && !(hi->rates_mask & snd_rate_bit(params->rate)))
            {
                return SND_ERR_BADFORMAT;
            }
            if (hi->period_min && params->period_size &&
                params->period_size < hi->period_min) return SND_ERR_BADARG;
            if (hi->period_max && params->period_size &&
                params->period_size > hi->period_max) return SND_ERR_BADARG;
            if (hi->buffer_min && params->buffer_size &&
                params->buffer_size < hi->buffer_min) return SND_ERR_BADARG;
            if (hi->buffer_max && params->buffer_size &&
                params->buffer_size > hi->buffer_max) return SND_ERR_BADARG;
        }
    }

    int rc = snd_active->pcm->hw_params(snd_active->priv, stream, params);
    if (rc == SND_OK) snd_cur_params[stream] = *params;
    return rc;
}

int snd_pcm_get_params(uint32_t stream, snd_pcm_params_t *out)
{
    if (stream >= SND_STREAM_COUNT || !out) return SND_ERR_BADARG;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    *out = snd_cur_params[stream];
    return SND_OK;
}

int snd_pcm_prepare(uint32_t stream)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->prepare)
        return SND_ERR_NODEV;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    return snd_active->pcm->prepare(snd_active->priv, stream);
}

int snd_pcm_start(uint32_t stream)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->trigger)
        return SND_ERR_NODEV;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    return snd_active->pcm->trigger(snd_active->priv, stream, SND_TRIGGER_START);
}

int snd_pcm_stop(uint32_t stream)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->trigger)
        return SND_ERR_NODEV;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    return snd_active->pcm->trigger(snd_active->priv, stream, SND_TRIGGER_STOP);
}

int snd_pcm_pause(uint32_t stream, int on)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->trigger)
        return SND_ERR_NODEV;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    return snd_active->pcm->trigger(snd_active->priv, stream,
                                    on ? SND_TRIGGER_PAUSE : SND_TRIGGER_RESUME);
}

int snd_pcm_is_open(uint32_t stream)
{
    if (stream >= SND_STREAM_COUNT) return 0;
    return snd_stream_open[stream];
}

uint32_t snd_pcm_pointer(uint32_t stream)
{
    if (stream >= SND_STREAM_COUNT) return 0xFFFFFFFFu;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->pointer)
        return 0xFFFFFFFFu;
    if (!snd_stream_open[stream]) return 0xFFFFFFFFu;
    return snd_active->pcm->pointer(snd_active->priv, stream);
}

int snd_pcm_write(uint32_t stream, const void *buf, uint32_t bytes)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->transfer)
        return SND_ERR_UNSUPPORTED;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    if (!buf || bytes == 0) return SND_ERR_BADARG;
    return snd_active->pcm->transfer(snd_active->priv, stream, buf, bytes);
}

int snd_pcm_read(uint32_t stream, void *buf, uint32_t bytes)
{
    if (stream >= SND_STREAM_COUNT) return SND_ERR_BADARG;
    if (!snd_active || !snd_active->pcm || !snd_active->pcm->transfer)
        return SND_ERR_UNSUPPORTED;
    if (!snd_stream_open[stream]) return SND_ERR_NOTOPEN;
    if (!buf || bytes == 0) return SND_ERR_BADARG;
    /* capture через тот же transfer: направление знает драйвер */
    return snd_active->pcm->transfer(snd_active->priv, stream, buf, bytes);
}

/* ---------------- микшер ---------------- */

uint32_t snd_mixer_get_channels(void)
{
    if (!snd_active || !snd_active->mixer || !snd_active->mixer->get_channels)
        return 0;
    return snd_active->mixer->get_channels(snd_active->priv);
}

int snd_mixer_set_volume(uint32_t channel, uint8_t left, uint8_t right)
{
    if (!snd_active || !snd_active->mixer || !snd_active->mixer->set_volume)
        return SND_ERR_NODEV;
    if (left > 100 || right > 100) return SND_ERR_BADARG;
    return snd_active->mixer->set_volume(snd_active->priv, channel, left, right);
}

int snd_mixer_get_volume(uint32_t channel, uint8_t *left, uint8_t *right)
{
    if (!snd_active || !snd_active->mixer || !snd_active->mixer->get_volume)
        return SND_ERR_NODEV;
    if (!left || !right) return SND_ERR_BADARG;
    return snd_active->mixer->get_volume(snd_active->priv, channel, left, right);
}

int snd_mixer_set_mute(uint32_t channel, int mute)
{
    if (!snd_active || !snd_active->mixer || !snd_active->mixer->set_mute)
        return SND_ERR_NODEV;
    return snd_active->mixer->set_mute(snd_active->priv, channel, mute);
}

int snd_mixer_get_mute(uint32_t channel, int *mute)
{
    if (!snd_active || !snd_active->mixer || !snd_active->mixer->get_mute)
        return SND_ERR_NODEV;
    if (!mute) return SND_ERR_BADARG;
    return snd_active->mixer->get_mute(snd_active->priv, channel, mute);
}

int snd_mixer_set_rec_source(uint32_t channel)
{
    if (!snd_active || !snd_active->mixer || !snd_active->mixer->set_rec_source)
        return SND_ERR_NODEV;
    return snd_active->mixer->set_rec_source(snd_active->priv, channel);
}
