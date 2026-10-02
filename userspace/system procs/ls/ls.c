#include "../../include/ustd.h"
#include "../../../include-kernel/lib.h"

// Алгоритм разбора Unix Timestamp в человеческую дату для консоли ARHeos
static void print_datetime(uint32_t timestamp) {
    if (timestamp == 0) {
        printf("00-00-0000 00:00  ");
        return;
    }

    // Базовые константы времени
    uint32_t seconds = timestamp % 60;
    uint32_t minutes = (timestamp / 60) % 60;
    uint32_t hours   = (timestamp / 3600) % 24;
    uint32_t days    = timestamp / 86400;

    // Сдвиг по умолчанию для московского часового пояса (MSK = UTC+3)
    hours = (hours + 3) % 24;
    if (hours < 3) days++;

    // Расчет года (начиная с эпохи 1970)
    uint32_t year = 1970;
    while (1) {
        bool is_leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        uint32_t days_in_year = is_leap ? 366 : 365;

        if (days >= days_in_year) {
            days -= days_in_year;
            year++;
        } else {
            break;
        }
    }

    // Расчет месяца и числа внутри найденного года
    uint8_t month_lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        month_lengths[1] = 29;
    }

    uint32_t month = 0;
    while (days >= month_lengths[month]) {
        days -= month_lengths[month];
        month++;
    }

    uint32_t day = days + 1;
    month = month + 1;

    printf("%d%d-%d%d-%d %d%d:%d%d  ",
        day / 10, day % 10,
        month / 10, month % 10,
        year,
        hours / 10, hours % 10,
        minutes / 10, minutes % 10
    );
}

static void print_flags(uint32_t attr, uint32_t type) {
    if (type == 0x02 || (attr & FS_ATTR_DIRECTORY)) {
        printf("d");
    } else if (attr & FS_ATTR_SYSTEM) {
        printf("s");
    } else {
        printf("-");
    }

    if (attr & FS_ATTR_READONLY) {
        printf("r- ");
    } else {
        printf("rw ");
    }

    if (attr & FS_ATTR_HIDDEN) {
        printf("h ");
    } else {
        printf("- ");
    }
}

int main(int argc, char *argv[]) {
    const char *target_dir = "";
    bool long_format = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            long_format = true;
        } else {
            target_dir = argv[i];
        }
    }

    int dir_fd = open(target_dir, 1);
    if (dir_fd < 0) {
        printf("ls: cannot open directory '%s'\n", target_dir[0] == '\0' ? "./" : target_dir);
        return -1;
    }

    u_dirent_t entry;
    int files_count = 0;

    while (u_readdir(dir_fd, &entry) == 0) {
        if (entry.name[0] == '\0') {
            continue;
        }

        if (long_format) {
            u_stat_t st;
            st.size = 0;
            st.attributes = entry.attributes;
            st.create_time = 0;
            st.modify_time = 0;

            // Запрашиваем у VFS динамические метаданные (размер и время изменения)
            stat(entry.name, &st);

            // Флаги и типы берем из объединенного состояния st и entry
            print_flags(st.attributes, entry.type);

            // Выводим номер Инода (из entry) и точный размер файла (из st)
            printf("ino: %d  size: %d B  ", (uint32_t)entry.inode, (uint32_t)st.size);

            // Выводим человеческую дату последней модификации из ext2
            print_datetime(st.modify_time);

            // Печатаем имя файла
            printf("%s", entry.name);

            if (entry.type == 0x02 || (st.attributes & FS_ATTR_DIRECTORY)) {
                printf("/");
            }
            printf("\n");
        } else {
            if (entry.type == 0x02 || (entry.attributes & FS_ATTR_DIRECTORY)) {
                printf("%s/  ", entry.name);
            } else {
                printf("%s  ", entry.name);
            }
        }
        files_count++;
    }

    if (files_count > 0 && !long_format) {
        printf("\n");
    }

    close(dir_fd);
    return 0;
}
