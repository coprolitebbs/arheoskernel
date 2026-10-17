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

//Гор - системный монитор

#include "../../include/ustd.h"


int main(int argc, char *argv[]){
    (void)argc;
    (void)argv;
    while(1) {
        //u_printf(400, 550, 0x00E0E0E0, "App live, ticks: %d", count++);
        yield();
    }

    return 0;
}
