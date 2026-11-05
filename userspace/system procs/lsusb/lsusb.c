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

#include "../../include/ustd.h"
#include "../../include/udevices.h"

//Формат вывода — плотный, без огромных отступов, чтобы читалось и на 80-колоночном VGA, и в pipe
static void print_hex4(uint32_t v)
{
    const char *hx = "0123456789abcdef";
    char b[5];
    b[0] = hx[(v >> 12) & 0xF];
    b[1] = hx[(v >>  8) & 0xF];
    b[2] = hx[(v >>  4) & 0xF];
    b[3] = hx[ v        & 0xF];
    b[4] = '\0';
    printf("%s", b);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    int n = u_usb_count();
    if (n < 0)
    {
        printf("lsusb: kernel returned error\n");
        return 1;
    }
    if (n == 0)
    {
        printf("lsusb: no USB devices registered\n");
        return 0;
    }

    for (int i = 0; i < n; i++)
    {
        u_usb_dev_t d;
        if (u_usb_get_dev(i, &d) != 0)
        {
            printf("lsusb: idx %d unavailable\n", i);
            continue;
        }

        printf("Bus 000 Device %03d: ID ", i + 1);
        print_hex4(d.vid);
        printf(":");
        print_hex4(d.pid);

        if (d.manufacturer[0]) printf(" %s", d.manufacturer);
        else
        {
            const char *v = u_usb_vendor_name(d.vid);
            if (v) printf(" %s", v);
        }

        if (d.product[0]) printf(" %s", d.product);
        printf(" (class %02x, port %d)", d.dev_class, d.port);

        if (d.serial[0]) printf(" sn:%s", d.serial);
        if (d.block_id != 0xFFFFFFFF) printf(" [usb%d]", d.block_id);

        printf("\n");

    }

    printf("\n%d device%s found.\n", n, (n == 1) ? "" : "s");
    return 0;
}
