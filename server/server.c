#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "config_parser.h"
#include "../libmysyslog1/libmysyslog.h"
#include <sys/types.h>
#include <sys/stat.h>

#define BUFFER_SIZE 1024

volatile sig_atomic_t shutdown_flag;

void sig_handler(int signum) {
    shutdown_flag = 1;
}

void run_as_daemon() {
    pid_t process_id = fork();
    if (process_id < 0) exit(EXIT_FAILURE);
    if (process_id > 0) exit(EXIT_SUCCESS);

    setsid();
    umask(0);

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

int check_user_permission(const char *user) {
    FILE *config_file = fopen("/etc/myRPC/users.conf", "r");
    if (!config_file) {
        mysyslog("Failed to open users.conf", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("Failed to open users.conf");
        return 0;
    }

    char config_line[256];
    int permission_granted = 0;

    while (fgets(config_line, sizeof(config_line), config_file) != NULL) {
        config_line[strcspn(config_line, "\n")] = '\0';

        if (config_line[0] == '#' || strlen(config_line) == 0)
            continue;

        if (strcmp(config_line, user) == 0) {
            permission_granted = 1;
            break;
        }
    }

    fclose(config_file);
    return permission_granted;
}

int run_system_command(const char *cmd, char *output_file, char *error_file) {
    char full_cmd[BUFFER_SIZE];
    snprintf(full_cmd, BUFFER_SIZE, "%s >%s 2>%s", cmd, output_file, error_file);
    int result = system(full_cmd);
    return WEXITSTATUS(result);
}

int main() {
    run_as_daemon();

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    Config server_config = parse_config("/etc/myRPC/myRPC.conf");

    int server_port = server_config.port;
    int is_stream_socket = strcmp(server_config.socket_type, "stream") == 0;

    mysyslog("Server starting...", INFO, 0, 0, "/var/log/myrpc.log");

    int main_socket;
    if (is_stream_socket) {
        main_socket = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        main_socket = socket(AF_INET, SOCK_DGRAM, 0);
    }

    if (main_socket < 0) {
        mysyslog("Socket creation failed", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("Socket creation failed");
        return 1;
    }

    int socket_option = 1;
    if (setsockopt(main_socket, SOL_SOCKET, SO_REUSEADDR, &socket_option, sizeof(socket_option)) < 0) {
        mysyslog("setsockopt failed", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("setsockopt failed");
        close(main_socket);
        return 1;
    }

    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(main_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        mysyslog("Bind failed", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("Bind failed");
        close(main_socket);
        return 1;
    }

    if (is_stream_socket) {
        listen(main_socket, 5);
        mysyslog("Server listening (stream)", INFO, 0, 0, "/var/log/myrpc.log");
    } else {
        mysyslog("Server listening (datagram)", INFO, 0, 0, "/var/log/myrpc.log");
    }

    while (!shutdown_flag) {
        char data_buffer[BUFFER_SIZE];
        int bytes_received;

        if (is_stream_socket) {
            addr_len = sizeof(client_addr);
            int client_socket = accept(main_socket, (struct sockaddr*)&client_addr, &addr_len);
            if (client_socket < 0) {
                mysyslog("Accept failed", ERROR, 0, 0, "/var/log/myrpc.log");
                perror("Accept failed");
                continue;
            }

            bytes_received = recv(client_socket, data_buffer, BUFFER_SIZE, 0);
            if (bytes_received <= 0) {
                close(client_socket);
                continue;
            }
            data_buffer[bytes_received] = '\0';

            mysyslog("Received request", INFO, 0, 0, "/var/log/myrpc.log");

            char *client_user = strtok(data_buffer, ":");
            char *client_cmd = strtok(NULL, "");
            if (client_cmd) {
                while (*client_cmd == ' ')
                    client_cmd++;
            }

            char server_response[BUFFER_SIZE];

            if (check_user_permission(client_user)) {
                mysyslog("User allowed", INFO, 0, 0, "/var/log/myrpc.log");

                char tmp_output[] = "/tmp/myRPC_XXXXXX.stdout";
                char tmp_error[] = "/tmp/myRPC_XXXXXX.stderr";
                mkstemp(tmp_output);
                mkstemp(tmp_error);

                int cmd_result = run_system_command(client_cmd, tmp_output, tmp_error);
                server_response[0] = (cmd_result == 0) ? '0' : '1';
                server_response[1] = ':';

                FILE *output_fp = fopen(tmp_output, "r");
                if (output_fp) {
                    size_t bytes_read = fread(server_response + 2, 1, BUFFER_SIZE - 3, output_fp);
                    server_response[0] = '0';
                    server_response[1] = ':';
                    server_response[bytes_read + 2] = '\0';
                    fclose(output_fp);
                    mysyslog("Command executed successfully", INFO, 0, 0, "/var/log/myrpc.log");
                } else {
                    strcpy(server_response, "1:Error reading command output");
                    mysyslog("Error reading stdout file", ERROR, 0, 0, "/var/log/myrpc.log");
                }

                remove(tmp_output);
                remove(tmp_error);

            } else {
                snprintf(server_response, BUFFER_SIZE, "1:User '%s' is not allowed", client_user);
                mysyslog("User not allowed", WARN, 0, 0, "/var/log/myrpc.log");
            }

            mysyslog("Sending response to client", INFO, 0, 0, "/var/log/myrpc.log");
            mysyslog(server_response, INFO, 0, 0, "/var/log/myrpc.log");
            send(client_socket, server_response, strlen(server_response), 0);
            close(client_socket);

        } else {
            addr_len = sizeof(client_addr);
            bytes_received = recvfrom(main_socket, data_buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, &addr_len);
            if (bytes_received <= 0) {
                continue;
            }
            data_buffer[bytes_received] = '\0';

            mysyslog("Received request", INFO, 0, 0, "/var/log/myrpc.log");

            char *client_user = strtok(data_buffer, ":");
            char *client_cmd = strtok(NULL, "");
            if (client_cmd) {
                while (*client_cmd == ' ')
                    client_cmd++;
            }

            char server_response[BUFFER_SIZE];

            if (check_user_permission(client_user)) {
                mysyslog("User allowed", INFO, 0, 0, "/var/log/myrpc.log");

                char tmp_output[] = "/tmp/myRPC_XXXXXX.stdout";
                char tmp_error[] = "/tmp/myRPC_XXXXXX.stderr";
                mkstemp(tmp_output);
                mkstemp(tmp_error);

                int cmd_result = run_system_command(client_cmd, tmp_output, tmp_error);
                server_response[0] = (cmd_result == 0) ? '0' : '1';
                server_response[1] = ':';

                FILE *output_fp = fopen(tmp_output, "r");
                if (output_fp) {
                    size_t bytes_read = fread(server_response + 2, 1, BUFFER_SIZE - 3, output_fp);
                    server_response[0] = '0';
                    server_response[1] = ':';
                    server_response[bytes_read + 2] = '\0';
                    fclose(output_fp);
                    mysyslog("Command executed successfully", INFO, 0, 0, "/var/log/myrpc.log");
                } else {
                    strcpy(server_response, "1:Error reading command output");
                    mysyslog("Error reading stdout file", ERROR, 0, 0, "/var/log/myrpc.log");
                }

                remove(tmp_output);
                remove(tmp_error);

            } else {
                snprintf(server_response, BUFFER_SIZE, "1:User '%s' is not allowed", client_user);
                mysyslog("User not allowed", WARN, 0, 0, "/var/log/myrpc.log");
            }

            sendto(main_socket, server_response, strlen(server_response), 0, (struct sockaddr*)&client_addr, addr_len);
        }
    }

    close(main_socket);
    mysyslog("Server stopped", INFO, 0, 0, "/var/log/myrpc.log");
    return 0;
}
