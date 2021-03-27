#include <unistd.h>
#include <stdio.h>
#include <stdlib.h> 
#include <string.h> 
#include <netdb.h> 
#include <netinet/in.h>
#include <arpa/inet.h> 
#include <sys/socket.h> 
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>

class tcp_t {
public:
    int tcp_recv(int conn, char *buffer, int max_length) {
        int length = read(conn, buffer, max_length);
		return length;
    }

    int tcp_send(int conn, const char *buffer, int length) {
        int sent = send(conn, buffer, length, 0);
        return sent;
    }
};

class tcp_client_t: public tcp_t {
public:
    tcp_client_t(): tcp_t() {}

    int request_connection(int remote_addr, int remote_port) {
        int conn = 0; 
        if ((conn = socket(AF_INET, SOCK_STREAM, 0)) < 0) { 
            perror ("Socket creation failed"); 
            return -1; 
        } 

        int opt = 1;
        setsockopt(conn, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in serv_addr; 
        memset(&serv_addr, '0', sizeof(serv_addr)); 
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = remote_addr;
        serv_addr.sin_port = htons(remote_port);
    
        if (connect(conn, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) { 
            perror ("Connection failed");
            struct in_addr ip_addr;
            ip_addr.s_addr = remote_addr;
            printf ("\tDestination: %s:%d\n", inet_ntoa(ip_addr), remote_port);
            close(conn);
            return -1; 
        }
        fcntl(conn, F_SETFL, O_NONBLOCK);

        return conn;
    }
};

class tcp_server_t: public tcp_t {
public:
    int sockfd;
    struct sockaddr_in address;
    int local_addr, local_port;

    tcp_server_t(int local_addr_, int local_port_): tcp_t() {
        local_addr = local_addr_;
        local_port = local_port_;

        if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
            perror("socket failed");
            exit(EXIT_FAILURE);
        }

        int opt = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
            perror("setsockopt failed");
            exit(EXIT_FAILURE);
        }
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = local_addr;
        address.sin_port = htons(local_port);

        if (bind(sockfd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("bind failed");
            exit(EXIT_FAILURE);
        }

        if (listen(sockfd, 128) < 0) {
            perror("listen failed");
            exit(EXIT_FAILURE);
        }

        fcntl(sockfd, F_SETFL, O_NONBLOCK);
    } 

    int accept_connection() {
        int conn;
        int addrlen = sizeof(address);
        conn = accept(sockfd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (conn >= 0)
            fcntl(conn, F_SETFL, O_NONBLOCK);
        return conn;
    }
};