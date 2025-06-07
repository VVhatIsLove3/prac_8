#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libmysyslog.h"

int mysyslog(const char* msg, int level, int driver, int format, const char* path) 
{
    if (msg == NULL || path == NULL) {
        return -1;
    }

    // Открытие файла журнала
    FILE *log_file = fopen(path, "a");
    if (log_file == NULL) {
        return -1;
    }

    // Форматирование временной метки
    time_t now = time(NULL);
    char timestamp[26];
    strncpy(timestamp, ctime(&now), sizeof(timestamp));
    timestamp[24] = '\0'; // Удаление символа новой строки

    // Получение строкового представления уровня
    const char *level_str;
    switch (level) {
        case DEBUG:    level_str = "DEBUG";    break;
        case INFO:     level_str = "INFO";     break;
        case WARN:     level_str = "WARN";     break;
        case ERROR:    level_str = "ERROR";    break;
        case CRITICAL: level_str = "CRITICAL"; break;
        default:       level_str = "UNKNOWN";  break;
    }

    // Запись в журнал в выбранном формате
    int result = 0;
    if (format == 0) {
        // Текстовый формат
        if (fprintf(log_file, "%s %s %d %s\n", timestamp, level_str, driver, msg) < 0) {
            result = -1;
        }
    } else {
        // Формат JSON
        if (fprintf(log_file, 
                   "{\"timestamp\":\"%s\",\"log_level\":\"%s\",\"driver\":%d,\"message\":\"%s\"}\n",
                   timestamp, level_str, driver, msg) < 0) {
            result = -1;
        }
    }

    fclose(log_file);
    return result;
}
