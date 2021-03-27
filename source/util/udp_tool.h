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

class udp_t {
public:
    int sockfd;
    struct sockaddr_in servaddr;

    udp_t(int addr, int port) {
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) { 
			perror("socket creation failed"); 
			exit(EXIT_FAILURE); 
		} 
		
		memset(&servaddr, 0, sizeof(servaddr)); 
		servaddr.sin_family = AF_INET;
		servaddr.sin_addr.s_addr = addr; 
		servaddr.sin_port = htons(port); 
    }
};

class udp_client_t: public udp_t {
public:
	udp_client_t(int remote_addr, int remote_port): udp_t(remote_addr, remote_port) {
		
	}

    int udp_send(char *buffer, int length) {
        int ret = sendto(sockfd, buffer, length, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
        return ret;
    }
};

class udp_server_t: public udp_t {
public:
	udp_server_t(int local_addr, int local_port): udp_t(local_addr, local_port) {
        if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0 ) { 
			perror("bind failed");
			exit(EXIT_FAILURE); 
		}
	}

    int udp_recv(char *buffer, int MAX_LENGTH) {
		struct sockaddr_in cliaddr;
		unsigned int len_addr;
        int length;
		length = recvfrom(sockfd, buffer, MAX_LENGTH, MSG_DONTWAIT, (struct sockaddr *)&cliaddr, &len_addr);
		return length;
	}
};