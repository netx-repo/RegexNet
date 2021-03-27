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

static void customize_copy_to_detector(struct http_msg msg, int id) {
	char *content = b_ptr(msg.chn->buf, -http_hdr_rewind(&msg));
	unsigned int length = msg.chn->buf->o;

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