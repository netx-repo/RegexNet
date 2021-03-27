#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

int http_get_unique_id(char *buffer) {
    char *ptr_id_str = strstr(buffer, "X-Unique-ID: ") + strlen("X-Unique-ID: ");
    if (ptr_id_str == NULL)
        return -1;

    int id;
    sscanf(ptr_id_str, "%d", &id);
    return id;
}

int http_get_server(char *buffer) {
    char *ptr_server_str = strstr(buffer, "X-Server: ") + strlen("X-Server: ");
    if (ptr_server_str == NULL)
        return -1;

    char server_str[32];
    sscanf(ptr_server_str, "%s", server_str);

    int server;
	inet_pton(AF_INET, server_str, &server);
    return server;
}