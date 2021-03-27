#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <map>

#include "util/udp_tool.h"
#include "util/tcp_tool.h"
#include "util/tool.h"

// load balancer addr, x5
#define ADDR_MANAGER "172.31.38.81"

#define PORT_COLLECTOR  9003
#define PORT_MANAGER    9004

#define MAX_LENGTH 100000
#define MALICIOUS_THRESHOLD 1

#define MESSAGE_REQUEST     0
#define MESSAGE_RESPONSE    1

using namespace std;

struct report_t {
    int length;
    int type;
    int id;
    long long timestamp;
	char buffer[MAX_LENGTH];
};

class collector_listen_t: public udp_server_t {
public:
    collector_listen_t(int listen_addr, int listen_port): udp_server_t(listen_addr, listen_port) {}

    report_t* get_report() {
        report_t *rpt = new report_t;
        rpt->length = udp_recv((char*)(&(rpt->type)), MAX_LENGTH);
        if (rpt->length < 1) {
			delete rpt;
			return NULL;
		}
        unsigned int offset = (char*)(&(rpt->buffer[0])) - (char*)(&(rpt->type));
        rpt->length = rpt->length - offset;

		return rpt;
	}
} collector_listen(INADDR_ANY, PORT_COLLECTOR);

map<int, report_t*> report_map;

int main() {
    //udp_client_t client(ip_str_to_int(ADDR_MANAGER), PORT_MANAGER);
    tcp_client_t client;
    while (true) {
        report_t *rpt = collector_listen.get_report();
        if (rpt != NULL) {
            if (rpt->type == MESSAGE_REQUEST) {
                report_t *req = rpt;
                report_map.insert(pair<int, report_t*>(req->id, req));
            }
            else {
                report_t *res = rpt;
                auto itr = report_map.find(res->id);
                if (itr == report_map.end())
                    continue;
                report_t *req = itr->second;
                report_map.erase(itr);
                long long latency = res->timestamp - req->timestamp;

                int conn = -1;
                while (conn < 0)
                    conn = client.request_connection(ip_str_to_int(ADDR_MANAGER), PORT_MANAGER);

                char metadata[128];
                memset(metadata, 0, 128);
                sprintf(metadata, "%32d; %64lld;", rpt->id, latency);
                client.tcp_send(conn, metadata, 128);

                client.tcp_send(conn, req->buffer, req->length);
                printf ("Report: %s\n", metadata);
                printf ("\tSent: %d\n", req->length);

                shutdown(conn, SHUT_WR);
                close(conn);

                delete req;
                delete res;
            }
        }
    }
}
