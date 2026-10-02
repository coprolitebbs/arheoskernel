#include "../../include/ustd.h"

int main(int argc, char *argv[]) {
    int count = 0;
    u_printf(510, 400, 0x00FFFFFF, "Userspace File Tester (cat) Initialized.");

    int res = mkdir("/u_dir");
    u_printf(510, 420, 0x00FFFF00, "u_mkdir status: %d", res);

    //Создаем и открываем файл на запись внутри новой папки
    int fd_w = open("/u_dir/u_test.txt", FS_OPEN_CREATE | FS_OPEN_WRITE);

    if (fd_w >= 0) {
        const char *msg = "OS_ALIVE";
        // Пишем 8 байт текстовых данных напрямую в секторы дискеты через DMA!
        int written = u_write(fd_w, msg, 8);
        u_printf(510, 430, 0x0000FF00, "u_write wrote: %d bytes", written);
        close(fd_w);
    } else {
        u_printf(510, 430, 0x00FF0000, "u_write error: cannot open file!");
    }

    //Открываем этот же файл на чтение, чтобы проверить физическую запись
    int fd_r = open("/u_dir/u_test.txt", FS_OPEN_READ);
    if (fd_r >= 0) {
        char read_buf[16];
        for(int i = 0; i < 16; i++) read_buf[i] = '\0';

        int read_bytes = u_read(fd_r, read_buf, 8);
        if (read_bytes > 0) {
            u_printf(510, 440, 0x0000FF00, "u_read success, content: %s", read_buf);
        }
        close(fd_r);
    }

    int res_unlink = unlink("/u_dir/u_test.txt");
    u_printf(510, 450, 0x00FFFF00, "u_unlink status: %d (0 = OK)", res_unlink);

    int res_rmdir = rmdir("/u_dir");
    u_printf(510, 460, 0x00FFFF00, "u_rmdir status: %d (0 = OK)", res_rmdir);

    /*for(int ii = 1; ii < 300000; ii++){
        u_yield();
    }*/

    // Открываем каталог диска /u_dir/
    int dir_fd = open("/", FS_OPEN_READ);

    if (dir_fd >= 0){
        u_dirent_t entry;
        int current_y = 80;

        u_printf(400, current_y, 0x00FFFF00, "Files on dir / :");
        current_y += 15;

        //Крутим цикл, пока системный вызов возвращает 0 (FS_OK)
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
    //Просто спим
    //while(1) {
        //u_printf(400, 550, 0x00E0E0E0, "App live, ticks: %d", count++);
        //yield();
    //}
    return 1;
}
