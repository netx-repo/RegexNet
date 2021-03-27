#ifndef CUSTOMIZE_H
#define CUSTOMIZE_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h> 
#include <sys/types.h> 
#include <sys/socket.h> 
#include <arpa/inet.h> 
#include <netinet/in.h> 
#include <pthread.h>

#include <common/buffer.h>
#include <proto/proto_http.h>
#include <proto/channel.h>

#define ADDR_DETECTOR	"172.31.38.81"
#define PORT_DETECTOR	9001 
#define MAXLENGTH		2048

static int customize_seqno = 0;
static time_t last_time = 0;
static int customize_throughput = 0;
static int customize_conn_fd = -1;

static void customize_send(const char *content, const unsigned int length) {
	/*if (length > 1000) {
		printf ("Forward potential malicious request\n");
	}*/
	
	struct timeval stop, start;

	if (customize_conn_fd < 0) {
		if ( (customize_conn_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0 ) { 
			perror("socket creation failed"); 
			exit(EXIT_FAILURE);
		}

		struct sockaddr_in servaddr;
		memset(&servaddr, 0, sizeof(servaddr)); 
		servaddr.sin_family = AF_INET;
		servaddr.sin_addr.s_addr = inet_addr(ADDR_DETECTOR);
		servaddr.sin_port = htons(PORT_DETECTOR);

		gettimeofday(&start, NULL);
		if (connect(customize_conn_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
			perror("connect failed"); 
			exit(EXIT_FAILURE);
		}
		gettimeofday(&stop, NULL);
		printf("Connect latency %lu us\n", (stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec); 
	}

	int length_n = htonl(length);
	if (write(customize_conn_fd, &length_n, sizeof(length_n)) < 0) {
		perror ("Send length failed"); 
	}
	if (write(customize_conn_fd, content, length) < 0) {
		perror ("Send content failed"); 
	}
}

static void customize_copy_to_detector(struct http_msg msg, int id) {
	char *content = b_ptr(msg.chn->buf, -http_hdr_rewind(&msg));
	unsigned int length = msg.chn->buf->o;
	customize_send(content, length);

	if (id > customize_seqno){
		customize_seqno = id;
		++customize_throughput;
		time_t current_time = time(NULL);
		if (current_time - last_time > 0) {
			if (last_time > 0) {
				for (int i = last_time + 1; i < current_time; ++i) {
					printf ("Throughput at %d: 0\n", i);
				}
			}
			printf ("Throughput at %d: %d\n", current_time, customize_throughput);
			fflush(stdout);
			customize_throughput = 0;
			last_time = current_time;
		}
	}
}

#endif