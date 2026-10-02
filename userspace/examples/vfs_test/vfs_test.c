#include "../../include/ustd.h"

int main(int argc, char *argv[]) {
    u_printf(400, 50, 0x00FF00FF, "Userspace VFS Reader Running");

    // Открываем корневой каталог дискеты FAT12 (флаг 1 - FS_OPEN_READ из вашего ядра)
    int dir_fd = open("/", FS_OPEN_READ);


    if (dir_fd >= 0) {
        u_dirent_t entry;
        int current_y = 80;

        u_printf(400, current_y, 0x00FFFF00, "Files on dir / :");
        current_y += 15;

        // Крутим цикл, пока системный вызов возвращает 0 (FS_OK)
        while (u_readdir(dir_fd, &entry) == 0) {
            u_printf(400, current_y, 0x00FFFFFF, "Ino: %d  Name: %s", entry.inode, entry.name);
            current_y += 12;
            if (current_y > 500) break;
        }

        close(dir_fd);
        u_printf(400, 70, 0x00FFFF00, "Closed");
    } else {
        u_printf(400, 80, 0x00FF0000, "Error: Cannot open root dir!");
    }


    // Уходим в дежурный цикл
    //int count = 0;
    //while(1) {
        //u_printf(400, 550, 0x00E0E0E0, "App live, ticks: %d", count++);
        //yield();
    //}

    return 2;
}

