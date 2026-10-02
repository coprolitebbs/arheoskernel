#include "../include/ustd.h"
#include <stdbool.h>
#include "../include/syscall.h"


//Общая функция запуска пользовательского приложения с корректным завершением и очисткой ресурсов
void __attribute__((naked)) _start(void){
    __asm__ __volatile__(
        "xor %%ebp, %%ebp\n\t"     //Обнуляем EBP по стандартам ABI (конец трассировки)
        "movl (%%esp), %%eax\n\t"   //Считываем argc прямо с вершины стека (не сдвигая ESP)
        "leal 4(%%esp), %%ebx\n\t"  //Вычисляем адрес массива указателей argv[] (он на 4 байта ниже)

        // Передаем параметры Си-функции cdecl способом через стек Ring 3
        "pushl %%ebx\n\t"           //Вторым аргументом main заталкиваем argv
        "pushl %%eax\n\t"           //Первым аргументом main заталкиваем argc

        "call main\n\t"            //Вызываем Си-функцию main! Результат (0) вернется в EAX

        //Очищаем стек Ring 3 от двухpush-ей параметров main (восстанавливаем ESP на +8 байт)
        "addl $8, %%esp\n\t"

        //ПомещаемSYS_EXIT строго в регистр EAX
        "movl %0, %%eax\n\t"
        "int $0x80\n\t"            //Дёргаем прерывание ядра

        "1:\n\t"
        "jmp 1b\n\t"               //Защитный барьер зависания
        :
        : "i"(SYS_EXIT)
        : "memory"
    );
    //u_printf(400, 570, 0x00FFFFFF, "exited");
}

void u_draw_char(uint32_t x, uint32_t y, char c, uint32_t color){
    __asm__ __volatile__("int $0x80"::"a"(SYS_DRAW_CHAR),"b"(x),"c"(y),"d"(c),"S"(color):"memory");
}

void u_exit(void){
    __asm__ __volatile__("int $0x80"::"a"(SYS_EXIT):"memory");
    for(;;);
}

void yield(void){
    __asm__ __volatile__("int $0x80"::"a"(SYS_YIELD):"memory");
}

//Самодельный printf, пишущий на экран по координатам X и Y
void u_printf(int x, int y, uint32_t color, const char *format, ...) {
    if (!format) return;

    __builtin_va_list args;
    __builtin_va_start(args, format);

    int current_x = x;
    const char *ptr = format;

    while (*ptr != '\0') {
        if (*ptr == '%') {
            ptr++;
            if (*ptr == 'c') {
                // Нативное извлечение символа
                char char_val = (char)__builtin_va_arg(args, int);
                u_draw_char(current_x, y, char_val, color);
                current_x += 10;
            }
            else if (*ptr == 's') {
                // Нативное извлечение строки
                const char *str_val = __builtin_va_arg(args, const char *);
                if (str_val) {
                    while (*str_val != '\0') {
                        u_draw_char(current_x, y, *str_val, color);
                        current_x += 10;
                        str_val++;
                    }
                }
            }
            else if (*ptr == 'd' || *ptr == 'x') {
                // Нативное извлечение чисел без смещения кадров стека
                uint32_t num_val = __builtin_va_arg(args, uint32_t);
                char buf[32];

                int i = 0;
                uint32_t temp = num_val;
                int base = (*ptr == 'x') ? 16 : 10;

                if (temp == 0) buf[i++] = '0';
                while (temp > 0) {
                    int r = temp % base;
                    buf[i++] = (r < 10) ? (r + '0') : (r - 10 + 'A');
                    temp /= base;
                }

                for (int j = i - 1; j >= 0; j--) {
                    u_draw_char(current_x, y, buf[j], color);
                    current_x += 10;
                }
            }
        } else {
            u_draw_char(current_x, y, *ptr, color);
            current_x += 10;
        }
        ptr++;
    }

    __builtin_va_end(args); // Закрываем фрейм vararg
}



int open(const char *path, uint32_t flags){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_OPEN),"b"(path),"c"(flags): "memory");
    return ret;
}

//Сложная вычитка с проверкой
int u_read(int fd, void *buf, uint32_t count){
    int ret;
    if(fd == STDIN){  //Обычный клавиатурный STDIN
        while (1){
            uint32_t local_fd = fd;
            void *local_buf = buf;
            uint32_t local_count = count;

            __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_READ),"b"(local_fd),"c"(local_buf),"d"(local_count):"memory","cc");

            if (ret == KBD_DRIVER_NOT_READY) {
                yield(); // yield() может портить регистры, но на следующем витке local_count их восстановит
                continue;
            }
            break;
        }
    } else  //Обычное чтение с файла
        __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_READ),"b"(fd),"c"(buf),"d"(count):"cc","memory");

    return ret;
}


int u_write(int fd, const void *buf, uint32_t count){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_WRITE),"b"(fd),"c"(buf),"d"(count):"memory");
    return ret;
}

int mkdir(const char *path){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_MKDIR),"b"(path):"memory");
    return ret;
}

int u_readdir(int fd, u_dirent_t *entry){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_READDIR),"b"(fd),"c"(entry):"memory");
    return ret;
}

int close(int fd){
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_CLOSE),"b"(fd):"memory");
    return ret;
}

int unlink(const char *path) {
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_UNLINK),"b"(path):"memory");
    return ret;
}

int rmdir(const char *path) {
    int ret;
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_RMDIR),"b"(path):"memory");
    return ret;
}

void clear_screen(void){
    __asm__ __volatile__("int $0x80" : : "a"(SYS_CLEAR_SCREEN) : "memory");
}


int u_spawn(const char *path, const char *cmdline){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_SPAWN), "b"(path), "c"(cmdline) : "memory");
    return ret;
}

int u_halt_os(void){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_HALT_OS) : "memory");
    return ret;
}


int u_wait_deity(int pid){
    int ret;
    //uint32_t count = 0;
    while (1) {
        __asm__ __volatile__(
            "int $0x80"
            : "=a"(ret)
            : "a"(SYS_WAIT_PID), "b"(pid)
            : "memory", "cc"
        );

        //u_printf(400, 550, 0x00E0E0E0, "App live, ticks: %d", count++);
        //Если ядро говорит, что порожденный процесс еще активен (-2)
        if (ret == PROCESS_IS_ACTIVE){
            yield(); //Добровольно уступаем процессор, пока ребенок работает
            continue;
        }
        break;
    }
    return ret;
}


int chdir(const char *path){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_CHDIR), "b"(path) : "memory");
    return ret;
}

int getcwd(char *buf, uint32_t max_len){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_GETCWD), "b"(buf), "c"(max_len) : "memory");
    return ret;
}

int stat(const char *path, u_stat_t *st){
    int ret;
    //SYS_STAT (номер 16) передает указатель на путь в EBX, а указатель на структуру метаданных в ECX
    __asm__ __volatile__("int $0x80":"=a"(ret):"a"(SYS_STAT),"b"(path),"c"(st):"memory");
    return ret;
}

int seek(int fd, uint32_t position){
    int ret;
    //Транслируем локальный fd процесса в глобальный через fds, если вызов идет из Ring 3,
    //но так как в syscalls.c вызов vfs_seek принимает file_d (локальный), шлем его напрямую
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_SEEK), "b"(fd), "c"(position) : "memory");
    return ret;
}

int tell(int fd){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_TELL), "b"(fd) : "memory");
    return ret;
}



int set_wallpaper(const void *buffer, uint32_t size){
    int ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(SYS_SET_WALLPAPER), "b"(buffer), "c"(size) : "memory");
    return ret;
}

static void n_put_str(const char *str){
    if (!str) return;
    uint32_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    if (len > 0) {
        u_write(1, str, len); //Пишем строго в стандартный поток вывода процессов!
    }
}


//Канонический потоковый printf с нативной защитой стека GCC через __builtin_va
void printf(const char *format, ...){
    if (!format) return;
    __builtin_va_list args;
    __builtin_va_start(args, format);
    const char *ptr = format;
    char char_buf[2] = {0, 0};

    while (*ptr != '\0'){
        if (*ptr == '%'){
            ptr++;
            if (*ptr == 'c'){
                char char_val = (char)__builtin_va_arg(args, int);
                char_buf[0] = char_val;
                n_put_str(char_buf);
            }
            else if (*ptr == 's'){
                const char *str_val = __builtin_va_arg(args, const char *);
                if (str_val) {
                    n_put_str(str_val);
                } else {
                    n_put_str("(null)");
                }
            }
            else if (*ptr == 'd' || *ptr == 'x'){
                uint32_t num_val = __builtin_va_arg(args, uint32_t);
                char num_buf[12];
                int i = 0;
                uint32_t temp = num_val;
                int base = (*ptr == 'x') ? 16 : 10;

                if (temp == 0) num_buf[i++] = '0';
                while (temp > 0){
                    int r = temp % base;
                    num_buf[i++] = (r < 10) ? (r + '0') : (r - 10 + 'A');
                    temp /= base;
                }

                char rev_buf[12];
                int rev_idx = 0;
                for (int j = i - 1; j >= 0; j--){
                    rev_buf[rev_idx++] = num_buf[j];
                }
                rev_buf[rev_idx] = '\0';
                n_put_str(rev_buf);
            }
        } else {
            char_buf[0] = *ptr;
            n_put_str(char_buf);
        }
        ptr++;
    }

    __builtin_va_end(args);

}


bool fileexists(const char *path){
    if (!path) return false;
    u_stat_t st;
    //Вызываем ваш системный вызов stat
    int res = stat(path, &st);
    //Если res != 0 (ядро вернуло ошибку, файла нет на диске), выходим
    if (res != 0) {
        return false;
    }
    //Проверяем поле attributes согласно флагам из ustd.h
    //Если это папка (FS_ATTR_DIRECTORY), то для fileexists это false
    if ((st.attributes & FS_ATTR_DIRECTORY) == FS_ATTR_DIRECTORY) {
        return false;
    }
    //Во всех остальных случаях, если объект существует и это не папка — это файл
    return true;
}


bool directoryexists(const char *path){
    if (!path) return false;
    u_stat_t st;
    int res = stat(path, &st);
    //Если папки нет на диске или путь некорректен
    if (res != 0){
        return false;
    }
    //Строго проверяем флаг директории
    return ((st.attributes & FS_ATTR_DIRECTORY) == FS_ATTR_DIRECTORY);
}
