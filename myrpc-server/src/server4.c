#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include "config_parser.h"
#include "libmysyslog.h"

/* Êîíñòàíòû ðàçìåðà */
enum {
    BUFFER_SIZE = 1024,
    MAX_USERS = 32,
    MAX_USERNAME_LEN = 31,
    MAX_PATH_LEN = 256
};

/* Êîíñòàíòû ïóòåé */
static const char CONFIG_FILE[] = "/etc/myRPC/server.conf";
static const char USERS_FILE[] = "/etc/myRPC/allowed_users.conf";
static const char LOG_FILE[] = "/var/log/myrpc.log";
static const char TEMPLATE_STDOUT[] = "/tmp/myRPC_XXXXXX.stdout";
static const char TEMPLATE_STDERR[] = "/tmp/myRPC_XXXXXX.stderr";
static const char PID_FILE[] = "/var/run/myrpc.pid";

volatile sig_atomic_t server_running = 1;

void handle_signal(int sig) {
    server_running = 0;
}

struct UserDB {
    char users[MAX_USERS][MAX_USERNAME_LEN + 1];  // +1 äëÿ íóëü-òåðìèíàòîðà
    int count;
};

void daemonize() {
    pid_t pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        exit(EXIT_FAILURE);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0) {
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    umask(0);
    chdir("/");

    for (int x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
        close(x);
    }

    openlog("myrpc", LOG_PID, LOG_DAEMON);
}

void create_pidfile() {
    FILE* f = fopen(PID_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

int load_users(struct UserDB* db) {
    FILE* file = fopen(USERS_FILE, "r");
    if (!file) {
        mysyslog("Failed to open users file", ERROR, 0, 0, LOG_FILE);
        return 0;
    }

    char line[MAX_USERNAME_LEN + 2]; // +2 äëÿ \n è \0
    db->count = 0;

    while (fgets(line, sizeof(line), file) && db->count < MAX_USERS) {
        line[strcspn(line, "\n")] = 0;
        if (line[0] != '#' && line[0] != '\0') {
            strncpy(db->users[db->count], line, MAX_USERNAME_LEN);
            db->users[db->count][MAX_USERNAME_LEN] = '\0';
            db->count++;
        }
    }

    fclose(file);
    return 1;
}

int is_user_allowed(const struct UserDB* db, const char* username) {
    for (int i = 0; i < db->count; i++) {
        if (strcmp(db->users[i], username) == 0) {
            return 1;
        }
    }
    return 0;
}

int execute_system_command(const char* command, char* output, size_t output_size) {
    char temp_out[MAX_PATH_LEN];
    char temp_err[MAX_PATH_LEN];

    strcpy(temp_out, TEMPLATE_STDOUT);
    strcpy(temp_err, TEMPLATE_STDERR);

    int fd_out = mkstemp(temp_out);
    int fd_err = mkstemp(temp_err);
    if (fd_out < 0 || fd_err < 0) {
        mysyslog("Failed to create temp files", ERROR, 0, 0, LOG_FILE);
        return -1;
    }
    close(fd_out);
    close(fd_err);

    char cmd[BUFFER_SIZE];
    snprintf(cmd, sizeof(cmd), "%s > %s 2> %s", command, temp_out, temp_err);

    int ret = system(cmd);
    if (ret != 0) {
        remove(temp_out);
        remove(temp_err);
        return -1;
    }

    FILE* f_out = fopen(temp_out, "r");
    if (f_out) {
        size_t bytes_read = fread(output, 1, output_size - 1, f_out);
        output[bytes_read] = '\0';
        fclose(f_out);
    }

    FILE* f_err = fopen(temp_err, "r");
    if (f_err) {
        size_t bytes_read = fread(output + strlen(output), 1,
            output_size - strlen(output) - 1, f_err);
        output[strlen(output) + bytes_read] = '\0';
        fclose(f_err);
    }

    remove(temp_out);
    remove(temp_err);

    return 0;
}

int setup_server_socket(int port, int is_stream) {
    int sock_type = SOCK_DGRAM;
    if (is_stream) {
        sock_type = SOCK_STREAM;
    }

    int sock = socket(AF_INET, sock_type, 0);
    if (sock < 0) {
        mysyslog("Socket creation failed", ERROR, 0, 0, LOG_FILE);
        return -1;
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(sock);
        mysyslog("setsockopt failed", ERROR, 0, 0, LOG_FILE);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        mysyslog("Bind failed", ERROR, 0, 0, LOG_FILE);
        return -1;
    }

    if (is_stream && listen(sock, 5) < 0) {
        close(sock);
        mysyslog("Listen failed", ERROR, 0, 0, LOG_FILE);
        return -1;
    }

    return sock;
}

void process_stream_request(int client_sock, const struct sockaddr_in* cli_addr,
    const struct UserDB* db) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
        return;
    }

    buffer[bytes_received] = '\0';
    mysyslog("Request received", INFO, 0, 0, LOG_FILE);

    char* username = strtok(buffer, ":");
    char* command = strtok(NULL, "");
    char response[BUFFER_SIZE] = { 0 };

    if (!username || !command) {
        strcpy(response, "Invalid request format");
    }
    else if (!is_user_allowed(db, username)) {
        snprintf(response, sizeof(response), "Access denied for user: %s", username);
        mysyslog("Access denied", WARN, 0, 0, LOG_FILE);
    }
    else if (execute_system_command(command, response, sizeof(response))) {
        strcpy(response, "Command execution failed");
        mysyslog("Command failed", ERROR, 0, 0, LOG_FILE);
    }
    else {
        mysyslog("Command executed", INFO, 0, 0, LOG_FILE);
    }

    send(client_sock, response, strlen(response), 0);
}

void process_datagram_request(int sock, const struct UserDB* db) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    ssize_t bytes_received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
        (struct sockaddr*)&client_addr, &client_len);
    if (bytes_received <= 0) {
        return;
    }

    buffer[bytes_received] = '\0';
    mysyslog("Request received", INFO, 0, 0, LOG_FILE);

    char* username = strtok(buffer, ":");
    char* command = strtok(NULL, "");
    char response[BUFFER_SIZE] = { 0 };

    if (!username || !command) {
        strcpy(response, "Invalid request format");
    }
    else if (!is_user_allowed(db, username)) {
        snprintf(response, sizeof(response), "Access denied for user: %s", username);
        mysyslog("Access denied", WARN, 0, 0, LOG_FILE);
    }
    else if (execute_system_command(command, response, sizeof(response))) {
        strcpy(response, "Command execution failed");
        mysyslog("Command failed", ERROR, 0, 0, LOG_FILE);
    }
    else {
        mysyslog("Command executed", INFO, 0, 0, LOG_FILE);
    }

    sendto(sock, response, strlen(response), 0,
        (struct sockaddr*)&client_addr, client_len);
}

int main() {
    daemonize();
    create_pidfile();

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    Config config = parse_config(CONFIG_FILE);
    if (config.port <= 0 || config.port > 65535) {
        mysyslog("Invalid port configuration", ERROR, 0, 0, LOG_FILE);
        return 1;
    }

    struct UserDB user_db;
    if (!load_users(&user_db)) {
        mysyslog("Failed to load users", ERROR, 0, 0, LOG_FILE);
        return 1;
    }

    int is_stream = (strcmp(config.socket_type, "stream") == 0);
    int server_sock = setup_server_socket(config.port, is_stream);
    if (server_sock < 0) {
        return 1;
    }

    mysyslog("Server started", INFO, 0, 0, LOG_FILE);

    while (server_running) {
        if (is_stream) {
            struct sockaddr_in cli_addr;
            socklen_t addr_len = sizeof(cli_addr);
            int client_sock = accept(server_sock, (struct sockaddr*)&cli_addr, &addr_len);
            if (client_sock >= 0) {
                process_stream_request(client_sock, &cli_addr, &user_db);
                close(client_sock);
            }
        }
        else {
            process_datagram_request(server_sock, &user_db);
        }
    }

    close(server_sock);
    remove(PID_FILE);
    mysyslog("Server stopped", INFO, 0, 0, LOG_FILE);
    return 0;
}
