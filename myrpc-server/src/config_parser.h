#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#define DEFAULT_PORT 8080

typedef struct {
    int port;
    char socket_type[64];
} Config;  // Убрано поле user

Config parse_config(const char *filename);

#endif // CONFIG_PARSER_H
