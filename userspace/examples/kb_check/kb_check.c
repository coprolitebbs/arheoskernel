#include "../../include/ustd.h"

int main(int argc, char *argv[]){

    clear_screen();

    // Выводим стартовое приветствие прямо в верхнее окно логов терминала через STDOUT!
    printf("==================================================\n");
    printf(" ARheOS TTY Cooked Core - Stream Console Mode  \n");
    printf("==================================================\n");
    //Русский шрифт в CP866
    printf("\x8D\xA0\xA1\xA5\xE0\xA8\xE2\xA5 \xE2\xA5\xAA\xE1\xE2 \xA8 \xAD\xA0\xA6\xAC\xA8\xE2\xA5 \x85\xAD\xE2\xA5\xE0 \xA4\xAB\xEF \xAF\xE0\xAE\xA2\xA5\xE0\xAA\xA8 \xAB\xAE\xA3\xA0:\n\n");

    char line_buffer[128];
    uint32_t iteration_count = 0;

    while (1){
        // Полностью зануляем буфер перед чтением
        for (int i = 0; i < 128; i++){
            line_buffer[i] = 0;
        }
        // Задача уходит в глубокий сон ядра до нажатия Enter
        int bytes_received = u_read(STDIN, line_buffer, 127);

        // Проверка
        if (bytes_received >= 0){
            iteration_count++; //Посчитали готовую строку лога
            //Выводим данные последовательным потоком
            printf("[Iter: %d] u_read returned: %d bytes\n", iteration_count, bytes_received);
            printf("  Raw dump (HEX codes): ");
            int limit = (bytes_received > 8) ? 8 : bytes_received;
            for (int i = 0; i < limit; i++){
                uint32_t char_code = (uint32_t)((unsigned char)line_buffer[i]);
                printf("0x%x ", char_code);
            }
            printf("\n");

            printf("  String text: \"%s\"\n\n", line_buffer);

        } else {
            //Если прилетел ложный 0 или ошибка - уступаем квант времени
            yield();
        }
    }

    return 0;
}
