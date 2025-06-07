#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "libmysyslog.h"

#define BUFFER_SIZE 1024
#define LOG_PATH "/var/log/myrpc.log"
#define MIN_PORT 1024
#define MAX_PORT 65535

typedef struct {
    char* command;
    char* server_ip;
    int port;
    int use_tcp;
} ClientConfig;

void show_help() {
    printf("Remote Command Client\n");
    printf("Usage: rpc_client [OPTIONS]\n");
    printf("Options:\n");
    printf("  -c, --command CMD    Command to execute on server\n");
    printf("  -h, --host IP        Server IP address\n");
    printf("  -p, --port PORT      Server port (%d-%d)\n", MIN_PORT, MAX_PORT);
    printf("  -s, --stream         Use TCP (default)\n");
    printf("  -d, --dgram          Use UDP\n");
    printf("      --help           Show this help\n");
}

int parse_args(int argc, char* argv[], ClientConfig* config) {
    config->use_tcp = 1; // TCP ïî óìîë÷àíèþ

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--command") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: Missing command argument\n");
                return -1;
            }
            config->command = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--host") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: Missing host argument\n");
                return -1;
            }
            config->server_ip = argv[++i];
        }
        else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: Missing port argument\n");
                return -1;
            }
            config->port = atoi(argv[++i]);
            if (config->port < MIN_PORT || config->port > MAX_PORT) {
                fprintf(stderr, "Port must be between %d and %d\n", MIN_PORT, MAX_PORT);
                return -1;
            }
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stream") == 0) {
            config->use_tcp = 1;
        }
        else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dgram") == 0) {
            config->use_tcp = 0;
        }
        else if (strcmp(argv[i], "--help") == 0) {
            show_help();
            return 1;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (!config->command || !config->server_ip || !config->port) {
        fprintf(stderr, "Missing required arguments\n");
        show_help();
        return -1;
    }
    return 0;
}

int create_socket(int use_tcp) {
    int sock = socket(AF_INET, use_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        mysyslog("Socket creation failed", ERROR, 0, 0, LOG_PATH);
    }
    return sock;
}

int connect_tcp(int sock, struct sockaddr_in* addr) {
    if (connect(sock, (struct sockaddr*)addr, sizeof(*addr))) {
        perror("connect");
        mysyslog("TCP connection failed", ERROR, 0, 0, LOG_PATH);
        return -1;
    }
    mysyslog("TCP connected", INFO, 0, 0, LOG_PATH);
        return 0;
}

void send_request(int sock, const char* request, struct sockaddr_in* addr, int use_tcp) {
    ssize_t sent = use_tcp
        ? send(sock, request, strlen(request), 0)
        : sendto(sock, request, strlen(request), 0, (struct sockaddr*)addr, sizeof(*addr));

    if (sent != (ssize_t)strlen(request)) {
        perror(sent < 0 ? "send" : "incomplete send");
        mysyslog("Send failed", ERROR, 0, 0, LOG_PATH);
        close(sock);
        exit(EXIT_FAILURE);
    }
}

void receive_response(int sock, char* buffer, int use_tcp) {
    ssize_t received;
    struct sockaddr_in udp_addr;
    socklen_t udp_len = sizeof(udp_addr);

    received = use_tcp
        ? recv(sock, buffer, BUFFER_SIZE - 1, 0)
        : recvfrom(sock, buffer, BUFFER_SIZE - 1, 0, (struct sockaddr*)&udp_addr, &udp_len);

    if (received < 0) {
        perror("recv");
        mysyslog("Receive failed", ERROR, 0, 0, LOG_PATH);
        close(sock);
        exit(EXIT_FAILURE);
    }

    buffer[received] = '\0';

    if (!use_tcp && received > 0) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &udp_addr.sin_addr, ip_str, sizeof(ip_str));
        printf("UDP response from %s:%d\n", ip_str, ntohs(udp_addr.sin_port));
    }
}

int main(int argc, char* argv[]) {
    ClientConfig config = { 0 };
    if (parse_args(argc, argv, &config) != 0) {
        return EXIT_FAILURE;
    }

    struct passwd* user = getpwuid(getuid());
    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "%s: %s", user->pw_name, config.command);

    mysyslog("Client started", INFO, 0, 0, LOG_PATH);

    int sock = create_socket(config.use_tcp);
    if (sock < 0) return EXIT_FAILURE;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(config.port)
    };
    if (inet_pton(AF_INET, config.server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        mysyslog("Invalid IP", ERROR, 0, 0, LOG_PATH);
        close(sock);
        return EXIT_FAILURE;
    }

    if (config.use_tcp && connect_tcp(sock, &addr) != 0) {
        close(sock);
        return EXIT_FAILURE;
    }

    send_request(sock, request, &addr, config.use_tcp);

    char response[BUFFER_SIZE];
    receive_response(sock, response, config.use_tcp);

    printf("Response: %s\n", response);
    mysyslog("Response received", INFO, 0, 0, LOG_PATH);

    close(sock);
    return EXIT_SUCCESS;
}
