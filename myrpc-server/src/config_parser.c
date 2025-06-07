#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "config_parser.h"
#include "libmysyslog.h"

#define MAX_STRING_LEN 64
#define LOG_PATH "/var/log/myrpc.log"

static void trim_whitespace(char *str) {
    if (str == NULL) return;
    
    char *start = str;
    while (*start && (*start == ' ' || *start == '\t')) {
        start++;
    }
    
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end-- = '\0';
    }
    
    if (start != str) {
        memmove(str, start, end - start + 2);
    }
}

Config parse_config(const char *filename) {
    Config config = {
        .port = DEFAULT_PORT,
        .socket_type = "stream"  // Только socket_type и port
    };

    if (filename == NULL) {
        mysyslog("Config filename is NULL, using defaults", WARN, 0, 0, LOG_PATH);
        return config;
    }

    FILE *file = fopen(filename, "r");
    if (!file) {
        mysyslog("Failed to open config file, using defaults", WARN, 0, 0, LOG_PATH);
        return config;
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        trim_whitespace(line);

        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "");

        if (key == NULL || value == NULL) {
            continue;
        }

        trim_whitespace(key);
        trim_whitespace(value);

        if (strcmp(key, "port") == 0) {
            int port = atoi(value);
            if (port > 0 && port <= 65535) {
                config.port = port;
            } else {
                mysyslog("Invalid port in config, using default", WARN, 0, 0, LOG_PATH);
            }
        } 
        else if (strcmp(key, "socket_type") == 0) {
            if (strcmp(value, "stream") == 0 || strcmp(value, "datagram") == 0) {
                strncpy(config.socket_type, value, MAX_STRING_LEN - 1);
                config.socket_type[MAX_STRING_LEN - 1] = '\0';
            } else {
                mysyslog("Invalid socket_type, using 'stream'", WARN, 0, 0, LOG_PATH);
            }
        }
    }

    fclose(file);
    return config;
}
