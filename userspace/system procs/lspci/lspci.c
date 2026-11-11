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

//Аналог lspci из Linux для ARheOS
//Формат вывода повторяет привычный вид:
//  00:01.1 0101: 8086:7010 IDE interface [82371SB PIIX3 IDE] (rev 01)
//Флаги:
//  -n  только числа (без имён классов/устройств)
//  -v  дополнительно: subsystem, IRQ, команда/статус, BAR-ы, мост
//  -x  hexdump первых 64 байт конфигурационного пространства
//  -h  справка

#include "../../include/ustd.h"
#include "../../include/udevices.h"
#include "../../include/udevices_struct.h"

//printf в ustd знает только %c %s %d %x, причём hex — заглавными и без
//паддинга. lspci нужны нижние регистры и фиксированная ширина,
//поэтому свои форматтеры

static void print_hex2(uint32_t v){
    const char *hx = "0123456789abcdef";
    char b[3];
    b[0] = hx[(v >> 4) & 0xF];
    b[1] = hx[ v       & 0xF];
    b[2] = '\0';
    printf("%s", b);
}

static void print_hex4(uint32_t v){
    const char *hx = "0123456789abcdef";
    char b[5];
    b[0] = hx[(v >> 12) & 0xF];
    b[1] = hx[(v >>  8) & 0xF];
    b[2] = hx[(v >>  4) & 0xF];
    b[3] = hx[ v        & 0xF];
    b[4] = '\0';
    printf("%s", b);
}

static void print_hex8(uint32_t v){
    print_hex4(v >> 16);
    print_hex4(v & 0xFFFF);
}

//Строка устройства в базовом формате lspci
static void print_dev_line(const u_pci_dev_t *d, int numeric){
    print_hex2(d->bus);
    printf(":");
    print_hex2(d->dev);
    printf(".");
    print_hex2(d->func);
    printf(" ");

    print_hex2(d->class_code);
    print_hex2(d->subclass);
    print_hex2(d->prog_if);
    printf(": ");

    print_hex4(d->vendor_id);
    printf(":");
    print_hex4(d->device_id);

    if (!numeric){
        const char *cls = u_pci_class_sub_name(d->class_code, d->subclass);
        if (cls) printf(" %s", cls);
        const char *dev = u_pci_device_name(d->vendor_id, d->device_id);
        if (dev) printf(" [%s]", dev);
    }

    printf(" (rev ");
    print_hex2(d->rev);
    printf(")");

    printf("\n");
}

//Декодирование одного BAR-а (по мотивам lspci -v)
static void print_bar(int i, uint32_t bar){
    if (bar == 0) return;

    printf("        Region %d: ", i);
    if (bar & 1){
        printf("I/O ports at ");
        print_hex4(bar & 0xFFFC);
        printf("h");
    } else {
        uint32_t type = (bar >> 1) & 3;
        if (type == 1){
            printf("unknown type at ");
            print_hex8(bar & 0xFFFFFFF0);
            printf("h");
        } else {
            printf("Memory at ");
            print_hex8(bar & 0xFFFFFFF0);
            printf("h (32-bit%s)", (bar & 8) ? ", prefetchable" : "");
            if (type == 2) printf(" (64-bit)");
        }
    }
    printf("\n");
}

//Дополнительный блок информации об устройстве (lspci -v)
static void print_dev_verbose(const u_pci_dev_t *d){
    printf("        Subsystem: ");
    print_hex4(d->subsys_vendor_id);
    printf(":");
    print_hex4(d->subsys_id);
    printf("\n");

    printf("        IRQ: ");
    if (d->irq == 0xFF) printf("unknown");
    else printf("%d", d->irq);
    printf("\n");

    printf("        Header: ");
    print_hex2(d->header_type);
    printf("h  Cmd: ");
    print_hex4(d->cmd);
    printf("h  Status: ");
    print_hex4(d->status);
    printf("h\n");

    for (int i = 0; i < PCI_BARS_MAX; i++) print_bar(i, d->bars[i]);

    if (d->rom_base != 0){
        printf("        Expansion ROM at ");
        print_hex8(d->rom_base & 0xFFFF0000);
        printf("h\n");
    }

    if (d->header_type == 0x01 && d->bridge_secondary != 0xFF){
        printf("        Bridge to: bus ");
        print_hex2(d->bridge_secondary);
        printf("\n");
    }
}

//Hexdump конфигурационного пространства:
//4 строки по 16 байт, байты внутри dword — от младшего к старшему
static void print_dev_x(const u_pci_dev_t *d){
    uint32_t bdf = PCI_BDF(d->bus, d->dev, d->func);

    for (uint32_t row = 0; row < 64; row += 16){
        print_hex2(row);
        printf(": ");
        for (uint32_t w = 0; w < 4; w++){
            uint32_t v = u_pci_read_cfg(bdf, (uint8_t)(row + w * 4));
            if (v == 0xFFFFFFFF){
                printf("<read failed>\n");
                return;
            }
            print_hex2(v & 0xFF);
            printf(" ");
            print_hex2((v >> 8) & 0xFF);
            printf(" ");
            print_hex2((v >> 16) & 0xFF);
            printf(" ");
            print_hex2((v >> 24) & 0xFF);
            printf(" ");
        }
        printf("\n");
    }
}

static void usage(const char *prog){
    printf("Usage: %s [-n] [-v] [-x] [-h]\n", prog);
    printf("  -n  numeric output only\n");
    printf("  -v  verbose (subsystem, irq, bars)\n");
    printf("  -x  hexdump of config space (first 64 bytes)\n");
    printf("  -h  this help\n");
}

int main(int argc, char *argv[]){
    int numeric  = 0;
    int verbose  = 0;
    int xdump    = 0;

    for (int i = 1; i < argc; i++){
        const char *a = argv[i];
        if (a[0] == '-'){
            for (int j = 1; a[j] != '\0'; j++){
                switch (a[j]){
                    case 'n': numeric = 1; break;
                    case 'v': verbose = 1; break;
                    case 'x': xdump  = 1; break;
                    case 'h': usage(argv[0]); return 0;
                    default:
                        printf("lspci: unknown option '-%c'\n", a[j]);
                        usage(argv[0]);
                        return 1;
                }
            }
        } else {
            printf("lspci: extra argument '%s'\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    int n = u_pci_count();
    if (n < 0){
        printf("lspci: kernel returned error\n");
        return 1;
    }
    if (n == 0){
        printf("lspci: no PCI devices found\n");
        return 0;
    }

    for (int i = 0; i < n; i++){
        u_pci_dev_t d;
        if (u_pci_get_dev(i, &d) != 0){
            printf("lspci: idx %d unavailable\n", i);
            continue;
        }

        print_dev_line(&d, numeric);
        if (xdump)   print_dev_x(&d);
        if (verbose) print_dev_verbose(&d);
    }

    printf("\n%d device%s found.\n", n, (n == 1) ? "" : "s");
    return 0;
}
