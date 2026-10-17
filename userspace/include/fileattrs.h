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

#ifndef FILEATTRS_H
#define FILEATTRS_H

//ошибки

#define FS_OK                 0
#define FS_NOT_FOUND         -1
#define FS_ALREADY_EXISTS    -2
#define FS_NO_SPACE          -3
#define FS_INVALID_PATH      -4
#define FS_ACCESS_DENIED     -5
#define FS_IO_ERROR          -6
#define FS_CORRUPTED         -7
#define FS_NOT_SUPPORTED     -8
#define FS_INVALID_HANDLE    -9
#define FS_EOF              -10
#define FS_TOO_MANY         -11
#define FS_DEBUG_CATCH        1

//Флаги открытия

#define FS_OPEN_READ      0x0001
#define FS_OPEN_WRITE     0x0002
#define FS_OPEN_CREATE    0x0004
#define FS_OPEN_APPEND    0x0008
#define FS_OPEN_TRUNCATE  0x0010

//Файловые атрибуты

#define FS_ATTR_REGULAR   0x0000
#define FS_ATTR_DIRECTORY 0x0001
#define FS_ATTR_READONLY  0x0002
#define FS_ATTR_HIDDEN    0x0004
#define FS_ATTR_SYSTEM    0x0008
#define FS_ATTR_ARCHIVE   0x0010


#endif
