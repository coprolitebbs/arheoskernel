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

#include "../include/udevices.h"
#include "../include/syscall.h"
#include "../include/udevices_struct.h"
#include <stddef.h>

int u_usb_count(void)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_USB_COUNT) : "memory");
    return ret;
}

int u_usb_get_dev(int idx, u_usb_dev_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_USB_GET_DEV), "b"(idx), "c"(out) : "memory");
    return ret;
}

int u_usb_rescan(void)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_USB_RESCAN) : "memory");
    return ret;
}


//Определения для системных вызовов USB

const char* u_usb_class_name(uint8_t c)
{
    switch (c)
    {
    case 0x00:
        return "Per-Interface";
    case 0x01:
        return "Audio";
    case 0x02:
        return "CDC Communications";
    case 0x03:
        return "HID";
    case 0x05:
        return "Physical";
    case 0x06:
        return "Image";
    case 0x07:
        return "Printer";
    case 0x08:
        return "Mass Storage";
    case 0x09:
        return "Hub";
    case 0x0A:
        return "CDC-Data";
    case 0x0B:
        return "Smart Card";
    case 0x0D:
        return "Content Security";
    case 0x0E:
        return "Video";
    case 0x0F:
        return "Personal Healthcare";
    case 0x10:
        return "Audio/Video";
    case 0xDC:
        return "Diagnostic";
    case 0xE0:
        return "Wireless";
    case 0xEF:
        return "Miscellaneous";
    case 0xFE:
        return "App Specific";
    case 0xFF:
        return "Vendor Specific";
    default:
        return "Unknown";
    }
}

const char* u_usb_vendor_name(uint32_t vid)
{
    switch (vid)
    {
    case 0x46f4:
        return "QEMU";
    case 0x0627:
        return "QEMU";
    case 0x8087:
        return "Intel";
    case 0x1d6b:
        return "Linux Foundation";
    case 0x046d:
        return "Logitech";
    case 0x0781:
        return "SanDisk";
    case 0x090c:
        return "Silicon Motion";
    case 0x13fe:
        return "Phison";
    case 0x0908:
        return "A-DATA";
    case 0x058f:
        return "Alcor";
    case 0x1307:
        return "Transcend";
    case 0x0930:
        return "Toshiba";
    case 0x1058:
        return "Western Digital";
    case 0x152d:
        return "JMicron";
    case 0x0bda:
        return "Realtek";
    default:
        return NULL;
        break;
    }
}


const char* u_pci_class_name(uint8_t c)
{
    switch (c)
    {
    case 0x00:
        return "Unclassified";
    case 0x01:
        return "Mass storage controller";
    case 0x02:
        return "Network controller";
    case 0x03:
        return "Display controller";
    case 0x04:
        return "Multimedia controller";
    case 0x05:
        return "Memory controller";
    case 0x06:
        return "Bridge";
    case 0x07:
        return "Communication controller";
    case 0x08:
        return "Generic system peripheral";
    case 0x09:
        return "Input device controller";
    case 0x0A:
        return "Docking station";
    case 0x0B:
        return "Processor";
    case 0x0C:
        return "Serial bus controller";
    case 0x0D:
        return "Wireless controller";
    case 0x0E:
        return "Intelligent controller";
    case 0x0F:
        return "Satellite communications controller";
    case 0x10:
        return "Encryption controller";
    case 0x11:
        return "Signal processing controller";
    case 0x12:
        return "Processing accelerator";
    case 0x13:
        return "Non-Essential Instrumentation";
    case 0x40:
        return "Co-processor";
    case 0xFF:
        return "Unassigned class";
    default:
        return "Unknown";
    }
}




//Определения для системных вызовов PCI

const char* u_pci_class_sub_name(uint8_t base, uint8_t sub)
{
    switch (base)
    {
    case 0x00:
        switch (sub)
        {
        case 0x00:
            return "Unclassified";
        case 0x01:
            return "VGA-compatible unclassified";
        }
        break;
    case 0x01:
        switch (sub)
        {
        case 0x00:
            return "SCSI storage controller";
        case 0x01:
            return "IDE interface";
        case 0x02:
            return "Floppy disk controller";
        case 0x03:
            return "IPI bus controller";
        case 0x04:
            return "RAID bus controller";
        case 0x05:
            return "ATA controller";
        case 0x06:
            return "SATA controller";
        case 0x07:
            return "SAS controller";
        case 0x08:
            return "Solid state controller";
        }
        break;
    case 0x02:
        switch (sub)
        {
        case 0x00:
            return "Ethernet controller";
        case 0x01:
            return "Token Ring controller";
        case 0x02:
            return "FDDI controller";
        case 0x03:
            return "ATM controller";
        case 0x04:
            return "ISDN controller";
        case 0x05:
            return "WorldFip controller";
        case 0x06:
            return "PICMG controller";
        case 0x80:
            return "Network controller";
        }
        break;
    case 0x03:
        switch (sub)
        {
        case 0x00:
            return "VGA compatible controller";
        case 0x01:
            return "XGA compatible controller";
        case 0x02:
            return "3D controller";
        case 0x80:
            return "Display controller";
        }
        break;
    case 0x04:
        switch (sub)
        {
        case 0x00:
            return "Multimedia video controller";
        case 0x01:
            return "Multimedia audio controller";
        case 0x02:
            return "Computer telephony interface";
        case 0x03:
            return "Audio device";
        }
        break;
    case 0x05:
        switch (sub)
        {
        case 0x00:
            return "RAM memory";
        case 0x01:
            return "Flash memory";
        case 0x02:
            return "TIMC";
        }
        break;
    case 0x06:
        switch (sub)
        {
        case 0x00:
            return "Host bridge";
        case 0x01:
            return "ISA bridge";
        case 0x02:
            return "EISA bridge";
        case 0x03:
            return "MicroChannel bridge";
        case 0x04:
            return "PCI bridge";
        case 0x05:
            return "PCMCIA bridge";
        case 0x06:
            return "NuBus bridge";
        case 0x07:
            return "CardBus bridge";
        case 0x08:
            return "RACEway bridge";
        case 0x09:
            return "Semi-transparent PCI-to-PCI bridge";
        case 0x0A:
            return "InfiniBand to PCI host bridge";
        }
        break;
    case 0x07:
        switch (sub)
        {
        case 0x00:
            return "Serial controller";
        case 0x01:
            return "Parallel controller";
        case 0x02:
            return "Multiport serial controller";
        case 0x03:
            return "Modem";
        case 0x04:
            return "GPIB controller";
        case 0x05:
            return "Smarts card controller";
        }
        break;
    case 0x08:
        switch (sub)
        {
        case 0x00:
            return "PIC";
        case 0x01:
            return "DMA controller";
        case 0x02:
            return "Timer";
        case 0x03:
            return "RTC";
        case 0x04:
            return "Generic hot-plug controller";
        }
        break;
    case 0x09:
        switch (sub)
        {
        case 0x00:
            return "Keyboard controller";
        case 0x01:
            return "Digitizer pen";
        case 0x02:
            return "Mouse controller";
        case 0x03:
            return "Scanner controller";
        case 0x04:
            return "Gameport controller";
        }
        break;
    case 0x0A:
        switch (sub)
        {
        case 0x00:
            return "Generic docking station";
        }
        break;
    case 0x0B:
        switch (sub)
        {
        case 0x00:
            return "386";
        case 0x01:
            return "486";
        case 0x02:
            return "Pentium";
        case 0x10:
            return "x86-64";
        case 0x20:
            return "Itanium";
        case 0x40:
            return "ARM";
        }
        break;
    case 0x0C:
        switch (sub)
        {
        case 0x00:
            return "FireWire (IEEE 1394)";
        case 0x01:
            return "ACCESS bus";
        case 0x02:
            return "SSA";
        case 0x03:
            return "USB controller";
        case 0x04:
            return "Fibre channel";
        case 0x05:
            return "SMBus";
        case 0x06:
            return "InfiniBand";
        case 0x07:
            return "IPMI SMIC interface";
        case 0x08:
            return "SERCOS interface";
        case 0x09:
            return "CANbus controller";
        }
        break;
    case 0x0D:
        switch (sub)
        {
        case 0x00:
            return "RF controller";
        case 0x01:
            return "Bluetooth controller";
        case 0x02:
            return "Broadband controller";
        case 0x10:
            return "WLAN controller";
        case 0x80:
            return "Wireless controller";
        }
        break;
    default:
        break;
    }
    return u_pci_class_name(base);
}


const char* u_pci_vendor_name(uint16_t vid)
{
    switch (vid)
    {
    case 0x1002:
        return "ATI/AMD";
    case 0x100B:
        return "National Semiconductor";
    case 0x1013:
        return "Cirrus Logic";
    case 0x1014:
        return "IBM";
    case 0x1022:
        return "AMD";
    case 0x102B:
        return "Matrox";
    case 0x1033:
        return "NEC";
    case 0x1039:
        return "Silicon Integrated Systems [SiS]";
    case 0x1045:
        return "OPTi";
    case 0x104C:
        return "Texas Instruments";
    case 0x1055:
        return "Efar";
    case 0x105A:
        return "Promise Technology";
    case 0x106B:
        return "Apple";
    case 0x10AD:
        return "Winbond";
    case 0x10B5:
        return "PLX Technology";
    case 0x10B7:
        return "3Com";
    case 0x10B9:
        return "Acer Labs [ALi]";
    case 0x10C3:
        return "Samsung";
    case 0x10DE:
        return "NVIDIA";
    case 0x10EC:
        return "Realtek";
    case 0x1102:
        return "Creative Labs";
    case 0x1106:
        return "VIA Technologies";
    case 0x1148:
        return "SysKonnect";
    case 0x115F:
        return "Popular Power";
    case 0x1166:
        return "Broadcom";
    case 0x1180:
        return "Ricoh";
    case 0x11AB:
        return "Marvell";
    case 0x124B:
        return "Olivetti";
    case 0x1260:
        return "Intersil";
    case 0x1274:
        return "Ensoniq";
    case 0x1275:
        return "Open Firmware";
    case 0x1283:
        return "Integrated Technology Express";
    case 0x12D8:
        return "Pericom Semiconductor";
    case 0x1317:
        return "Admtek";
    case 0x1345:
        return "Acard";
    case 0x1414:
        return "Microsoft";
    case 0x141D:
        return "Databook";
    case 0x15AD:
        return "VMware";
    case 0x168C:
        return "Atheros";
    case 0x17F3:
        return "RDC Semiconductor";
    case 0x1AF4:
        return "Red Hat (virtio)";
    case 0x1B36:
        return "Red Hat (QEMU)";
    case 0x1969:
        return "Atheros";
    case 0x19A2:
        return "Emulex";
    case 0x1DDE:
        return "Advanced Micro Devices";
    case 0x21B3:
        return "Sandboxed";
    case 0x2646:
        return "Kingston Technology";
    case 0x3388:
        return "Turtle Beach";
    case 0x5333:
        return "S3";
    case 0x8086:
        return "Intel";
    case 0x80EE:
        return "InnoTek/VirtualBox";
    case 0x8680:
        return "Bay Microsystems";
    case 0xC0A9:
        return "AMI";
    default:
        return NULL;
    }
}

//Определения для системных вызовов PCI

const char* u_pci_device_name(uint16_t vid, uint16_t did)
{
    switch (vid)
    {
    case 0x8086:
        switch (did)
        {
        case 0x100E:
            return "82540EM Gigabit Ethernet";
        case 0x1209:
            return "82559ER Fast Ethernet";
        case 0x1229:
            return "82557/8/9 Ethernet";
        case 0x1237:
            return "440FX - 82441FX PMC";
        case 0x2415:
            return "82801AA AC'97 Audio";
        case 0x2668:
            return "82801FB/FBM High Definition Audio";
        case 0x2922:
            return "82801IR/IO SATA";
        case 0x7000:
            return "82371SB PIIX3 ISA";
        case 0x7010:
            return "82371SB PIIX3 IDE";
        case 0x7020:
            return "82371SB PIIX3 USB";
        case 0x7110:
            return "82371AB PIIX4 ISA";
        case 0x7111:
            return "82371AB PIIX4 IDE";
        case 0x7112:
            return "82371AB PIIX4 USB";
        case 0x7113:
            return "82371AB PIIX4 ACPI";
        case 0x7190:
            return "440BX/ZX/DX - 82443BX Host";
        case 0x7191:
            return "440BX/ZX/DX - 82443BX AGP";
        case 0x7192:
            return "440BX/ZX/DX - 82443BX PCI";
        }
        break;
    case 0x1013:
        if (did == 0x00B8) return "CL-GD5446/GD5480 VGA";
        break;
    case 0x1234:
        if (did == 0x1111) return "Bochs/QEMU SVGA II";
        break;
    case 0x1B36:
        switch (did)
        {
        case 0x0001:
            return "PCI-PCI bridge";
        case 0x0008:
            return "PCI Express Root Port";
        case 0x000D:
            return "XHCI host controller";
        case 0x0010:
            return "QEMU NVM Express";
        }
        break;
    case 0x1AF4:
        switch (did)
        {
        case 0x1000:
            return "Virtio network device";
        case 0x1001:
            return "Virtio block device";
        case 0x1002:
            return "Virtio memory balloon";
        case 0x1003:
            return "Virtio console";
        case 0x1004:
            return "Virtio SCSI";
        case 0x1005:
            return "Virtio RNG";
        case 0x1041:
            return "Virtio block (modern)";
        case 0x1042:
            return "Virtio balloon (modern)";
        case 0x1049:
            return "Virtio network (modern)";
        case 0x1053:
            return "Virtio RNG (modern)";
        }
        break;
    case 0x10EC:
        if (did == 0x8139) return "RTL-8139/8139C/8139C+";
        break;
    case 0x15AD:
        if (did == 0x0774) return "Virtual SVGA II";
        break;
    case 0x80EE:
        if (did == 0xBEEF) return "VirtualBox Graphics Adapter";
        if (did == 0xCAFE) return "VirtualBox Guest Service";
        break;
    case 0x5333:
        if (did == 0x8811) return "Trio64 (PCI)";
        break;
    default:
        break;
    }
    return NULL;
}





int u_pci_count(void)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_PCI_COUNT) : "memory");
    return ret;
}

int u_pci_get_dev(int idx, u_pci_dev_t *out)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_PCI_GET_DEV), "b"(idx), "c"(out) : "memory");
    return ret;
}

uint32_t u_pci_read_cfg(uint32_t bdf, uint8_t off)
{
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret)
                         : "a"(SYS_PCI_READ_CFG), "b"(bdf), "c"((uint32_t)off) : "memory");
    return (uint32_t)ret;
}
