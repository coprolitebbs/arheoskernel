//Copyright (C) 2026 ARHeOS by Dmitry H
//
//This program is free software: you can redistribute it and/or modify
//it under the terms of the GNU General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.
//
//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//GNU General Public License for more details.
//
//You should have received a copy of the GNU General Public License
//along with this program. If not, see <https://www.gnu.org/licenses/>.

//ugraph.c — пользовательские обертки над SYS_GRAPH_INFO.
//Системный вызов отдает информацию об активном видеодрайвере графического
//слоя: карточку драйвера, текущий режим, fix-информацию и полный список
//режимов (для vesa_fb это VBE-таблица от загрузчика, для fbdrv-fallback —
//единственный boot-режим из bootinfo)

#include "../include/ugraph.h"
#include "../include/syscall.h"
#include <stddef.h>

int u_gr_driver_info(u_gr_driver_info_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_GRAPH_INFO), "b"(GRAPH_INFO_DRIVER), "c"(out) : "memory");
    return ret;
}

int u_gr_mode(uint32_t index, u_gr_mode_info_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_GRAPH_INFO), "b"(GRAPH_INFO_MODE), "c"(index), "d"(out) : "memory");
    return ret;
}

int u_gr_current_mode(u_gr_mode_info_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_GRAPH_INFO), "b"(GRAPH_INFO_CURRENT), "c"(out) : "memory");
    return ret;
}

int u_gr_fix_info(u_gr_fix_info_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_GRAPH_INFO), "b"(GRAPH_INFO_FIX), "c"(out) : "memory");
    return ret;
}

int u_gr_set_mode(uint32_t index)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_GRAPH_INFO), "b"(GRAPH_INFO_SET_MODE), "c"(index) : "memory");
    return ret;
}

const char *u_gr_visual_name(uint32_t visual)
{
    switch (visual)
    {
    case U_GR_VISUAL_MONO01:      return "mono01";
    case U_GR_VISUAL_MONO10:      return "mono10";
    case U_GR_VISUAL_PSEUDOCOLOR: return "pseudocolor";
    case U_GR_VISUAL_TRUECOLOR:   return "truecolor";
    case U_GR_VISUAL_DIRECTCOLOR: return "directcolor";
    default:                      return "unknown";
    }
}

const char *u_gr_type_name(uint32_t type)
{
    switch (type)
    {
    case U_GR_TYPE_PACKED_PIXELS: return "packed_pixels";
    case U_GR_TYPE_PLANES:        return "planes";
    case U_GR_TYPE_VGA_PLANES:    return "vga_planes";
    default:                      return "unknown";
    }
}
