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

//Локальное сравнение строк: lsusb.elf линкуется без u_lib_elf.o,
//где живёт strcmp
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == *b);
}

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

//printf в ARHeOS не поддерживает ширину/zero-pad (%03d печатает "3d"),
//поэтому все числовые поля печатаем вручную
static void print_hex2(uint32_t v)
{
    const char *hx = "0123456789abcdef";
    char b[3];
    b[0] = hx[(v >> 4) & 0xF];
    b[1] = hx[ v      & 0xF];
    b[2] = '\0';
    printf("%s", b);
}

static void print_dec3(int v)
{
    char b[4];
    if (v < 0) v = 0;
    if (v > 999) v = 999;
    b[0] = (char)('0' + (v / 100) % 10);
    b[1] = (char)('0' + (v / 10) % 10);
    b[2] = (char)('0' + v % 10);
    b[3] = '\0';
    printf("%s", b);
}

//Снимок реестра в статический массив (не на стеке — записи по ~100 байт)
static u_usb_dev_t snap_before[USB_REGISTRY_MAX];
static u_usb_dev_t snap_after[USB_REGISTRY_MAX];

static int take_snapshot(u_usb_dev_t *dst, int max)
{
    int n = u_usb_count();
    if (n < 0) return -1;
    if (n > max) n = max;
    for (int i = 0; i < n; i++)
    {
        if (u_usb_get_dev(i, &dst[i]) != 0) return i;   //что успели, то успели
    }
    return n;
}

//Есть ли в снимке устройство с тем же vid:pid на том же порту
static int snap_contains(const u_usb_dev_t *arr, int n, const u_usb_dev_t *d)
{
    for (int i = 0; i < n; i++)
    {
        if (arr[i].vid == d->vid && arr[i].pid == d->pid &&
            arr[i].port == d->port) return 1;
    }
    return 0;
}

static void print_list(int n)
{
    for (int i = 0; i < n; i++)
    {
        u_usb_dev_t d;
        if (u_usb_get_dev(i, &d) != 0)
        {
            printf("lsusb: idx %d unavailable\n", i);
            continue;
        }

        printf("Bus 000 Device ");
        print_dec3(i + 1);
        printf(": ID ");
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
        printf(" (class ");
        print_hex2(d.dev_class);
        printf(", port %d)", d.port);

        if (d.serial[0]) printf(" sn:%s", d.serial);
        if (d.block_id != 0xFFFFFFFF) printf(" [usb%d]", d.block_id);

        printf("\n");
    }

    printf("\n%d device%s found.\n", n, (n == 1) ? "" : "s");
}

int main(int argc, char *argv[])
{
    int do_update = 0;

    for (int i = 1; i < argc; i++)
    {
        if (streq(argv[i], "-update") || streq(argv[i], "--update"))
        {
            do_update = 1;
        }
        else
        {
            printf("usage: lsusb [-update]\n");
            printf("  lsusb         показать USB-устройства из реестра ядра\n");
            printf("  lsusb -update пересмотреть порты контроллеров (вставлено/\n");
            printf("                вынуто) и показать свежий список\n");
            return 1;
        }
    }

    if (!do_update)
    {
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
        print_list(n);
        return 0;
    }

    //--- lsusb -update: ручной hotplug через SYS_USB_RESCAN ---

    int n0 = take_snapshot(snap_before, USB_REGISTRY_MAX);

    printf("lsusb: rescanning USB ports...\n");

    int n = u_usb_rescan();
    if (n < 0)
    {
        printf("lsusb: rescan failed (%d)\n", n);
        printf("  (-1: USB driver not loaded, other: enumeration error)\n");
        return 1;
    }

    int n1 = take_snapshot(snap_after, USB_REGISTRY_MAX);
    if (n1 < 0) n1 = n;

    //Diff по (vid:pid, порт): что появилось и что исчезло с последнего списка
    int added = 0, removed = 0;
    for (int i = 0; i < n1; i++)
    {
        if (!snap_contains(snap_before, n0, &snap_after[i]))
        {
            printf("+ ");
            print_hex4(snap_after[i].vid);
            printf(":");
            print_hex4(snap_after[i].pid);
            printf(" new device on port %d (class ",
                   snap_after[i].port);
            print_hex2(snap_after[i].dev_class);
            printf(")\n");
            added++;
        }
    }
    for (int i = 0; i < n0; i++)
    {
        if (!snap_contains(snap_after, n1, &snap_before[i]))
        {
            printf("- ");
            print_hex4(snap_before[i].vid);
            printf(":");
            print_hex4(snap_before[i].pid);
            printf(" device removed from port %d\n", snap_before[i].port);
            removed++;
        }
    }

    if (added || removed)
        printf("lsusb: rescan done: +%d -%d\n", added, removed);
    else
        printf("lsusb: rescan done: no changes\n");

    if (n1 == 0)
    {
        printf("lsusb: no USB devices registered\n");
        return 0;
    }

    print_list(n1);
    return 0;
}
