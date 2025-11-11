#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libmysyslog.h"

int mysyslog(const char* log_message, int severity_level, int driver_id, int output_format, const char* log_path) {
    // Открытие файла журнала
    FILE *log_stream;
    log_stream = fopen(log_path, "a");
    if (log_stream == NULL) {
        return -1;
    }

    // Получение текущего времени
    time_t current_time;
    time(&current_time);
    char *time_string = ctime(&current_time);
    time_string[strlen(time_string) - 1] = '\0'; // Удаление символа новой строки

    // Преобразование уровня серьезности в строку
    const char *severity_text;
    switch (severity_level) {
        case DEBUG:    severity_text = "DEBUG"; break;
        case INFO:     severity_text = "INFO"; break;
        case WARN:     severity_text = "WARN"; break;
        case ERROR:    severity_text = "ERROR"; break;
        case CRITICAL: severity_text = "CRITICAL"; break;
        default:       severity_text = "UNKNOWN"; break;
    }

    // Запись в журнал в выбранном формате
    if (output_format == 0) {
        // Простой текстовый формат
        fprintf(log_stream, "%s %s %d %s\n", time_string, severity_text, driver_id, log_message);
    } else {
        // Формат JSON
        fprintf(log_stream,
               "{\"timestamp\":\"%s\",\"log_level\":\"%s\",\"driver\":%d,\"message\":\"%s\"}\n",
               time_string, severity_text, driver_id, log_message);
    }

    fclose(log_stream);
    return 0;
}
