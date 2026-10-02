//Сехмет - командный процессор

#include "../../include/ustd.h"
#include "../../include/umemory.h"
#include "../../include/uimage.h"
#include "../../../include-kernel/lib.h"
#include "../../include/uconfig.h"

#define MAX_CMD_LEN 256
#define MAX_ARGS 16
#define HELP_BUF_SIZE 254

#define MAX_ENV_PATHS 8
#define MAX_PATH_LEN  64

char startup_paths[MAX_ENV_PATHS][MAX_PATH_LEN];
int startup_paths_count = 0;

char greeting[128] = {0};
char greeting2[128] = {0};
char* appname_default = "\x91\xA5\xE5\xAC\xA5\xE2";
char appname[32]= {0};
char shell_prompt[64] = {0};
char help_path[128] = {0};
char help_notfound[64] = {0};
char f_execute_mess[32] = {0};
char wallp_load[32] = {0};
char f_cd_mess[40] = {0};
char exit_mess[50] = {0};
char awake_mess[20] = {0};
char awake_failed_mess[50] = {0};


void trim_newline(char *str){
    int i = 0;
    while (str[i] != '\0'){
        if (str[i] == '\n' || str[i] == '\r'){
            str[i] = '\0';
            break;
        }
        i++;
    }
}


int parse_command(char *cmdline, char *args[]){
    int argc = 0;
    int i = 0;
    int arg_start = -1;
    while (cmdline[i] != '\0'){
        if (cmdline[i] == ' ' || cmdline[i] == '\t'){
            if (arg_start != -1){
                cmdline[i] = '\0';
                args[argc++] = &cmdline[arg_start];
                arg_start = -1;
            }
        } else {
            if (arg_start == -1){
                arg_start = i;
            }
        }
        if (argc >= (MAX_ARGS - 1)) break;
        i++;
    }
    if (arg_start != -1 && argc < (MAX_ARGS - 1)){
        args[argc++] = &cmdline[arg_start];
    }
    args[argc] = NULL;
    return argc;
}



bool find_executable(const char *cmd_name, char *out_resolved_path, int max_out_len){
    char temp_buf[256];

    //Проверяем, не передан ли уже абсолютный/относительный явный путь (содержит '/')
    bool explicit_path = false;
    bool has_dot = false;
    for (int i = 0; cmd_name[i] != '\0'; i++){
        if (cmd_name[i] == '/') explicit_path = true;
        if (cmd_name[i] == '.') has_dot = true;
    }
    if (explicit_path){
        //Если указан явный путь, и в нем уже есть расширение (например, /bin/init.elf)
        strncpy(out_resolved_path, cmd_name, max_out_len - 1);
        out_resolved_path[max_out_len - 1] = '\0';
        if (fileexists(out_resolved_path)) return true;
        //Если расширения нет, пробуем сначала авто-подстановку .elf, затем .bin
        if (!has_dot) {
            //Пробуем .elf
            strcpy(temp_buf, cmd_name);
            strcat(temp_buf, ".elf");
            if (fileexists(temp_buf)) {
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
            //Пробуем .bin
            strcpy(temp_buf, cmd_name);
            strcat(temp_buf, ".bin");
            if (fileexists(temp_buf)) {
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
        }
        return false;
    }
    //Ищем по массиву путей поиска (startup_paths) из config.cfg
    for (int i = 0; i < startup_paths_count; i++){
        char base_path[128];
        strcpy(base_path, startup_paths[i]);

        int len = strlen(base_path);
        if (len > 0 && base_path[len - 1] != '/'){
            strcat(base_path, "/");
        }
        //Если пользователь ввел команду сразу с точкой
        if (has_dot) {
            strcpy(temp_buf, base_path);
            strcat(temp_buf, cmd_name);
            if (fileexists(temp_buf)) {
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
        } else {
            //Приоритетный поиск: Проверяем наличие .elf версии программы
            strcpy(temp_buf, base_path);
            strcat(temp_buf, cmd_name);
            strcat(temp_buf, ".elf");
            if (fileexists(temp_buf)){
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
            //Фолбек-поиск: Проверяем наличие старой .bin версии программы
            strcpy(temp_buf, base_path);
            strcat(temp_buf, cmd_name);
            strcat(temp_buf, ".bin");
            if (fileexists(temp_buf)){
                strncpy(out_resolved_path, temp_buf, max_out_len - 1);
                out_resolved_path[max_out_len - 1] = '\0';
                return true;
            }
        }
    }
    //Проверяем локальный путь (текущую рабочую директорию процесса CWD)
    char cwd[256];
    memset(cwd, 0, sizeof(cwd));
    getcwd(cwd, 255);

    int cwd_len = strlen(cwd);
    if (cwd_len > 0 && cwd[cwd_len - 1] != '/') {
        strcat(cwd, "/");
    }

    if (has_dot) {
        strcpy(temp_buf, cwd);
        strcat(temp_buf, cmd_name);
        if (fileexists(temp_buf)) {
            strncpy(out_resolved_path, temp_buf, max_out_len - 1);
            out_resolved_path[max_out_len - 1] = '\0';
            return true;
        }
    } else {
        //Проверяем локальный .elf
        strcpy(temp_buf, cwd);
        strcat(temp_buf, cmd_name);
        strcat(temp_buf, ".elf");
        if (fileexists(temp_buf)){
            strncpy(out_resolved_path, temp_buf, max_out_len - 1);
            out_resolved_path[max_out_len - 1] = '\0';
            return true;
        }

        //Проверяем локальный .bin
        strcpy(temp_buf, cwd);
        strcat(temp_buf, cmd_name);
        strcat(temp_buf, ".bin");
        if (fileexists(temp_buf)){
            strncpy(out_resolved_path, temp_buf, max_out_len - 1);
            out_resolved_path[max_out_len - 1] = '\0';
            return true;
        }
    }

    return false;
}



void load_config(void){
    cfg_file_t *cfg = cfg_open("/config/sekhmet/config.cfg", FS_OPEN_READ, true);
    if (cfg < 0) {
        strcpy(appname, appname_default);
        strcpy(shell_prompt, appname_default);
        strcpy(help_path, "/config/sekhmet/sekhmet.help");
        printf("%s: %s not found, keeping default parameters.\n",appname,help_path);
        strcpy(startup_paths[0], "/bin");
        startup_paths_count = 1;
        return;
    }

    printf("Loading config, please wait");

    //memset(appname, 0, 32);
    //memset(shell_prompt, 0, 64);
    char raw_prompt[64]={0};
    //memset(raw_prompt, 0, 64);

    char help_filename[64]={0};
    //memset(help_filename, 0, 64);

    //memset(help_notfound, 0, 64);
    //printf("here\n");
    //memset(greeting, 0, 128);
    //memset(greeting2, 0, 128);

    cfg_read_string(cfg, "main", "appname", appname_default, appname, 32);printf(".");

    cfg_read_string(cfg, "main", "prompt", "$appname", raw_prompt, 64);printf(".");
    cfg_read_string(cfg, "main", "greeting", "greeting err", greeting, 128);printf(".");
    cfg_read_string(cfg, "main", "greeting2", "greeting err", greeting2, 128);printf(".");

    cfg_read_string(cfg, "help", "filename", "sekhmet.hlp", help_filename, 64);printf(".");
    strcpy(help_path, "/config/sekhmet/");
    strcat(help_path, help_filename);

    cfg_read_string(cfg, "help", "notfound", "File not found", help_notfound, 64);printf(".");

    strrep(shell_prompt, raw_prompt, "$appname", appname, 64);printf(".");

    //Парсинг startup_path (Окружение путей поиска)
    char raw_paths_buf[256];

    memset(raw_paths_buf, 0, sizeof(raw_paths_buf));

    startup_paths_count = 0;
    cfg_read_string(cfg, "main", "startup_path", "/bin", raw_paths_buf, 256);printf(".");
    if (raw_paths_buf[0] != '\0'){
        //Используем strtok для разделения по ';'
        char *token = strtok(raw_paths_buf, ";");
        while (token != NULL && startup_paths_count < MAX_ENV_PATHS) {
            //Копируем токен в глобальный массив
            strncpy(startup_paths[startup_paths_count], token, MAX_PATH_LEN - 1);
            startup_paths[startup_paths_count][MAX_PATH_LEN - 1] = '\0';
            startup_paths_count++;

            token = strtok(NULL, ";");
        }
    }

    //Дополнительные сообщения
    cfg_read_string(cfg, "messages", "f_execute", "Unknown ritual or missing deity", f_execute_mess, 32);printf(".");
    cfg_read_string(cfg, "messages", "wallp_load", "Loading wallpaper...", wallp_load, 32);printf(".");
    cfg_read_string(cfg, "messages", "f_cd", "cd: no such file or directory", f_cd_mess, 40);printf(".");
    cfg_read_string(cfg, "messages", "exit_mess", "shell session dissolved. Looming into darkness", exit_mess, 50);printf(".");
    cfg_read_string(cfg, "messages", "awake_mess", "Awakening entity", awake_mess, 20);printf(".");
    cfg_read_string(cfg, "messages", "awake_failed", "Ritual failed. Cannot awake entity.", awake_failed_mess, 50);printf(".");

    //Загрузка обоев
    char wp_file[128];
    char wp_flag[32];
    memset(wp_file, 0, 128);
    yield();
    memset(wp_flag, 0, 32);

    cfg_read_string(cfg, "wallpaper", "file", "", wp_file, 128);printf(".");
    cfg_read_string(cfg, "wallpaper", "flag", "stretch", wp_flag, 32);printf(".");

    //printf("Loading wallpaper %s...\n",wp_file);

    if (wp_file[0] != '\0' && fileexists(wp_file)){
        printf("\n%s %s\n",wallp_load,wp_file);
        //Определяем численный режим (0 - Center, 1 - Tile, 2 - Stretch)
        uint32_t mode = 0; //По умолчанию Центр
        if (strcmp(wp_flag, "tile") == 0){
            mode = 1;
        } else if (strcmp(wp_flag, "stretch") == 0){
            mode = 2;
        }
        //Запрашиваем актуальные физические параметры экрана у ядра
        uint32_t scr_pitch = 0, scr_bpp = 0, scr_width = 0, scr_height = 0;
        get_fb_info(&scr_pitch, &scr_bpp, &scr_width, &scr_height);
        //Страховочный фолбек ядра, если видеодрайвер не вернул данные
        if (scr_width == 0 || scr_height == 0 || scr_bpp == 0){
            scr_width = 1024;
            scr_height = 768;
            scr_bpp = 32;
            scr_pitch = 1024 * 4;
        }
        //Вызываем библиотечный декодер TGA
        uint8_t *wp_buffer = load_tga_to_buffer(wp_file, scr_width, scr_height, scr_bpp, scr_pitch, mode);
        if (wp_buffer){
            //Рассчитываем точный объем ОЗУ на базе pitch фреймбуфера
            uint32_t wp_size_bytes = scr_height * scr_pitch;
            //Вызываем системный вызов ядра Ring 0 для маппинга обоев в память видеодрайвера
            int res = set_wallpaper(wp_buffer, wp_size_bytes);
            //Освобождаем тяжелый Userspace-буфер из графической кучи процесса
            free_pages(wp_buffer, wp_size_bytes);
            if (res == 0){
                clear_screen();
            }
        }

    }

    cfg_close(cfg);
    printf("\n");
}



//Функция вычитки и печати внешнего текстового файла справки
void show_external_help(void) {
    int fd = open(help_path, FS_OPEN_READ);
    if (fd < 0) {
        printf("%s: %s\n", appname, help_notfound);
        return;
    }

    char buffer[HELP_BUF_SIZE + 2];
    int bytes_read;

    //Поточно вычитываем файл справки чанками по 254 байта
    while (1) {
        memset(buffer, 0, HELP_BUF_SIZE + 2);
        bytes_read = u_read(fd, buffer, HELP_BUF_SIZE);
        if (bytes_read <= 0) {
            break;
        }
        buffer[bytes_read] = '\0';
        printf("%s", buffer);//Выводим текст в TTY поток консоли Сехмет
    }

    close(fd);
    printf("\n");
}



int main(int argc, char *argv[]){
    (void)argc;
    (void)argv;

    char cmd_buf[MAX_CMD_LEN];
    char *args[MAX_ARGS];
    char cwd_buf[256];

    load_config();

    chdir("/");

    printf("\n%s\n",greeting);
    printf("%s\n\n",greeting2);


    while (1){
        memset(cwd_buf, 0, 256);
        getcwd(cwd_buf, 255);
        printf("%s:%s", shell_prompt, cwd_buf);
        printf("> ");

        memset(cmd_buf, 0, MAX_CMD_LEN);
        int bytes_read = u_read(STDIN, cmd_buf, MAX_CMD_LEN - 1);

        if (bytes_read <= 0){
            yield();
            continue;
        }

        trim_newline(cmd_buf);
        if (cmd_buf[0] == '\0'){
            continue;
        }

        int cmd_argc = parse_command(cmd_buf, args);
        if (cmd_argc == 0){
            continue;
        }
        if (strcmp(args[0], "help") == 0){
            show_external_help();
        }
        else if (strcmp(args[0], "clear") == 0){
            clear_screen();
        }
        else if (strcmp(args[0], "cd") == 0){
            const char *target = "/"; //Если просто написать 'cd', прыгаем в корень
            if (cmd_argc >= 2) {
                target = args[1];
            }
            int res = chdir(target);
            if (res < 0) {
                printf("%s: %s\n",f_cd_mess,target);
            }
        }
        else if (strcmp(args[0], "echo") == 0){
            for (int i = 1; i < cmd_argc; i++){
                printf("%s ", args[i]);
            }
            printf("\n");
        }
        else if (strcmp(args[0], "exit") == 0){
            printf("%s: %s\n",appname,exit_mess);
            return 0;
        }
        else {
            char resolved_path[128];
            memset(resolved_path, 0, sizeof(resolved_path));
            //Запускаем сквозной поиск программы
            if (find_executable(args[0], resolved_path, 128)){
                //Перепаковываем аргументы для payload
                char args_payload[128];
                memset(args_payload, 0, 128);
                int payload_idx = 0;
                for (int i = 1; i < cmd_argc; i++){
                    for (int j = 0; args[i][j] != '\0' && payload_idx < 126; j++){
                        args_payload[payload_idx++] = args[i][j];
                    }
                    if (i < (cmd_argc - 1) && payload_idx < 126){
                        args_payload[payload_idx++] = ' ';
                    }
                }
                printf("[%s] %s: %s...\n", appname,awake_mess,resolved_path);
                int child_pid = u_spawn(resolved_path, args_payload);
                if (child_pid >= 0){
                    //printf("[\x91\xA5\xE5\xAC\xA5\xE2] Deity process manifested with PID: %d\n", child_pid);
                    u_wait_deity(child_pid);
                } else {
                    printf("%s: %s\n",appname,awake_failed_mess);
                }
            } else {
                // Если файл не найден ни в одной директории PATH, ни локально
                printf("%s: %s: %s\n", appname,f_execute_mess,args[0]);
            }

        }

    }

    return 0;
}
