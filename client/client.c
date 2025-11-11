#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "../libmysyslog1/libmysyslog.h"

#define BUFFER_SIZE 1024
#define RESPONSE_HEADER_LEN 2

void display_help() {
    printf("Usage: myrpc-client -h HOST -p PORT -c \"COMMAND\" [OPTIONS]\n");
    printf("Options:\n");
    printf("  -c, --command COMMAND    Command to execute (required)\n");
    printf("  -h, --host HOST          Server IP address (required)\n");
    printf("  -p, --port PORT          Server port (required)\n");
    printf("  -s, --stream             Use stream socket (default)\n");
    printf("  -d, --dgram              Use datagram socket\n");
    printf("      --help               Display this help and exit\n");
}

int is_valid_response(const char *resp) {
    if (strlen(resp) < RESPONSE_HEADER_LEN) {
        return 0;
    }

    if (resp[0] != '0' && resp[0] != '1') {
        return 0;
    }

    if (resp[1] != ':') {
        return 0;
    }

    return 1;
}

int main(int argc, char *argv[]) {
    char *user_command = NULL;
    char *server_address = NULL;
    int server_port = 0;
    int use_tcp = 1;
    int option;

    static struct option long_opts[] = {
        {"command", required_argument, 0, 'c'},
        {"host", required_argument, 0, 'h'},
        {"port", required_argument, 0, 'p'},
        {"stream", no_argument, 0, 's'},
        {"dgram", no_argument, 0, 'd'},
        {"help", no_argument, 0, 0},
        {0, 0, 0, 0}
    };

    while ((option = getopt_long(argc, argv, "c:h:p:sd", long_opts, NULL)) != -1) {
        switch (option) {
            case 'c':
                user_command = strdup(optarg);
                break;
            case 'h':
                server_address = strdup(optarg);
                break;
            case 'p':
                server_port = atoi(optarg);
                if (server_port <= 0 || server_port > 65535) {
                    fprintf(stderr, "Invalid port number\n");
                    return 1;
                }
                break;
            case 's':
                use_tcp = 1;
                break;
            case 'd':
                use_tcp = 0;
                break;
            case 0:
                display_help();
                return 0;
            default:
                display_help();
                return 1;
        }
    }

    if (!user_command || !server_address || server_port == 0) {
        fprintf(stderr, "Error: Missing required arguments\n");
        display_help();

        if (user_command) free(user_command);
        if (server_address) free(server_address);
        return 1;
    }

    struct passwd *user_info = getpwuid(getuid());
    if (!user_info) {
        fprintf(stderr, "Error: Could not get username\n");
        free(user_command);
        free(server_address);
        return 1;
    }
    const char *current_user = user_info->pw_name;

    char request_data[BUFFER_SIZE];
    int request_size = snprintf(request_data, BUFFER_SIZE, "%s:%s", current_user, user_command);
    if (request_size >= BUFFER_SIZE) {
        fprintf(stderr, "Error: Command too long\n");
        free(user_command);
        free(server_address);
        return 1;
    }

    mysyslog("Connecting to server...", INFO, 0, 0, "/var/log/myrpc.log");

    int client_sock = socket(AF_INET, use_tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (client_sock < 0) {
        mysyslog("Socket creation failed", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("Error");
        free(user_command);
        free(server_address);
        return 1;
    }

    struct sockaddr_in server_info = {
        .sin_family = AF_INET,
        .sin_port = htons(server_port)
    };

    if (inet_pton(AF_INET, server_address, &server_info.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server address\n");
        close(client_sock);
        free(user_command);
        free(server_address);
        return 1;
    }

    if (use_tcp) {
        if (connect(client_sock, (struct sockaddr*)&server_info, sizeof(server_info)) < 0) {
            mysyslog("Connection failed", ERROR, 0, 0, "/var/log/myrpc.log");
            perror("Error");
            close(client_sock);
            free(user_command);
            free(server_address);
            return 1;
        }

        if (send(client_sock, request_data, strlen(request_data), 0) < 0) {
            mysyslog("Send failed", ERROR, 0, 0, "/var/log/myrpc.log");
            perror("Error");
            close(client_sock);
            free(user_command);
            free(server_address);
            return 1;
        }

        char server_reply[BUFFER_SIZE] = {0};
        int reply_size = recv(client_sock, server_reply, BUFFER_SIZE - 1, 0);
        if (reply_size < 0) {
            mysyslog("Receive failed", ERROR, 0, 0, "/var/log/myrpc.log");
            perror("Error");
            close(client_sock);
            free(user_command);
            free(server_address);
            return 1;
        }
        server_reply[reply_size] = '\0';

        if (!is_valid_response(server_reply)) {
            printf("Invalid server response format: %s\n", server_reply);
            close(client_sock);
            free(user_command);
            free(server_address);
            return 1;
        }

        int status_code = server_reply[0] - '0';
        const char *command_output = server_reply + 2;

        if (status_code == 0) {
            printf("%s\n", command_output);
        } else {
            fprintf(stderr, "Error: %s\n", command_output);
        }
    } else {
        if (sendto(client_sock, request_data, strlen(request_data), 0,
                  (struct sockaddr*)&server_info, sizeof(server_info)) < 0) {
            mysyslog("Send failed", ERROR, 0, 0, "/var/log/myrpc.log");
            perror("Error");
            close(client_sock);
            free(user_command);
            free(server_address);
            return 1;
        }

        char server_reply[BUFFER_SIZE] = {0};
        socklen_t addr_size = sizeof(server_info);
        int reply_size = recvfrom(client_sock, server_reply, BUFFER_SIZE - 1, 0,
                                (struct sockaddr*)&server_info, &addr_size);
        if (reply_size < 0) {
            mysyslog("Receive failed", ERROR, 0, 0, "/var/log/myrpc.log");
            perror("Error");
            close(client_sock);
            free(user_command);
            free(server_address);
            return 1;
        }
        server_reply[reply_size] = '\0';

        if (!is_valid_response(server_reply)) {
            printf("Invalid server response format: %s\n", server_reply);
            close(client_sock);
            free(user_command);
            free(server_address);
            return 1;
        }

        int status_code = server_reply[0] - '0';
        const char *command_output = server_reply + 2;

        if (status_code == 0) {
            printf("%s\n", command_output);
        } else {
            fprintf(stderr, "Error: %s\n", command_output);
        }
    }

    mysyslog("Command executed", INFO, 0, 0, "/var/log/myrpc.log");

    close(client_sock);
    free(user_command);
    free(server_address);
    return 0;
}
