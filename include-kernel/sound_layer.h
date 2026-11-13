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


#ifndef SOUND_LAYER_H
#define SOUND_LAYER_H

#include "../drivers/include_drivers/driver_sound_layer.h"

//================= API слоя (реализовано в sound_layer.c) ==============
//Вызывается только из кода ядра. Драйверы получают слой неявно:
//их snd_driver_t регистрируется ядром после загрузки ELF-модуля

void init_sound_layer(void);

//реестр драйверов
int sound_layer_register_driver(snd_driver_t *drv);     //автопереключение по priority
int sound_layer_unregister_driver(const char *name);
int sound_layer_activate_driver(const char *name);
snd_driver_t *sound_layer_get_active(void);
uint32_t sound_layer_get_driver_count(void);

//PCM
int snd_pcm_open(uint32_t stream);
int snd_pcm_close(uint32_t stream);
int snd_pcm_get_hw_info(uint32_t stream, snd_pcm_hw_info_t *out);
int snd_pcm_set_params(uint32_t stream, const snd_pcm_params_t *params);
int snd_pcm_get_params(uint32_t stream, snd_pcm_params_t *out);
int snd_pcm_prepare(uint32_t stream);
int snd_pcm_start(uint32_t stream);
int snd_pcm_stop(uint32_t stream);
int snd_pcm_pause(uint32_t stream, int on);
int snd_pcm_is_open(uint32_t stream);
//позиция в DMA-кольце, байты; 0xFFFFFFFF = недоступна
uint32_t snd_pcm_pointer(uint32_t stream);
//PIO-передача (SND_FEAT_PIO); для DMA-драйверов вернёт SND_ERR_UNSUPPORTED:
//они сами читают свой кольцевой буфер, а слой даёт pointer() для синхронизации
int snd_pcm_write(uint32_t stream, const void *buf, uint32_t bytes);
int snd_pcm_read(uint32_t stream, void *buf, uint32_t bytes);

//микшер
uint32_t snd_mixer_get_channels(void);
int snd_mixer_set_volume(uint32_t channel, uint8_t left, uint8_t right);   //0..100
int snd_mixer_get_volume(uint32_t channel, uint8_t *left, uint8_t *right);
int snd_mixer_set_mute(uint32_t channel, int mute);
int snd_mixer_get_mute(uint32_t channel, int *mute);
int snd_mixer_set_rec_source(uint32_t channel);

#endif
