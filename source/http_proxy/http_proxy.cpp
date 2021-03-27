#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <fcntl.h>
#include <inttypes.h>

#include <chrono>
#include <deque>
#include <set>
#include <map>
#include <iterator>
#include <iostream>

#include "util/tool.h"
#include "util/udp_tool.h"
#include "util/tcp_tool.h"
#include "util/http_tool.h"

#define MAX_LENGTH 100000

#define CONCURRENCY_LIMIT 4
#define NUM_NODEJS      4
#define PORT_FRONTEND   8880
#define PORT_NODEJS_A   8881
#define PORT_NODEJS_B   8882
#define PORT_NODEJS_C   8883
#define PORT_NODEJS_D   8884
#define PORT_WARNING    9002

const char *ADDR_COLLECTOR = "127.0.0.1"; // localhost
#define PORT_COLLECTOR  9003

const char *ADDR_SANDBOX = "127.0.0.1"; // localhost
#define PORT_SANDBOX    8099

#define MESSAGE_REQUEST 0
#define MESSAGE_RESPONSE 1

using namespace std;

auto program_start_time = std::chrono::high_resolution_clock::now();

struct message_t {
    int length;

    int type;
    int id;
    long long timestamp;
    char buffer[MAX_LENGTH];
};

class frontend_t: public tcp_server_t {
public:
    frontend_t(int listen_addr, int listen_port): tcp_server_t(listen_addr, listen_port) {}

    int recv_request(int conn, message_t *req) {
        while (true) {
            int retval = tcp_recv(conn, req->buffer + req->length, MAX_LENGTH);
            if (retval > 0)
                req->length += retval;
            else
                break;
        }
        if (req->buffer[req->length - 2] == '\r' && req->buffer[req->length - 1] == '\n') {
            req->buffer[req->length] = '\0';
            req->id = http_get_unique_id(req->buffer);
            return 1;
        }
        else {
            return 0;
        }
    }

    int send_response(int conn, message_t* res) {
        int length = tcp_send(conn, res->buffer, res->length);
        return length;
    }
} frontend(INADDR_ANY, PORT_FRONTEND);

class backend_t: public tcp_client_t {
private:
    int server_addr;
    int server_port;
public:
    backend_t(int server_addr_, int server_port_):
        server_addr(server_addr_), server_port(server_port_) {}

    int request_connection() {
        return tcp_client_t::request_connection(server_addr, server_port);
    }

    int send_request(int conn, message_t *req) {
        return tcp_send(conn, req->buffer, req->length);
    }

    int recv_response(int conn, message_t *res, int id) {
        res->length = read(conn, res->buffer, MAX_LENGTH);
        if (res->length < 1) {
            return -1;
        }

        res->type = MESSAGE_RESPONSE;
        res->id = id;
        res->timestamp = chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - program_start_time).count();

        return res->length;
    }
};

class nodejs_t: public backend_t {
private:
    int pid;
    char *envp[3];
public:
    nodejs_t(int backend_addr, int backend_port): backend_t(backend_addr, backend_port) {
        envp[0] = new char[32];
        sprintf(envp[0], "NODE_ENV=production");
        envp[1] = new char[32];
        sprintf(envp[1], "PORT=%d", backend_port);
        envp[2] = NULL;

        pid = -1;
        restart();
    }

    void restart() {
        if (pid > -1)
            kill (pid, SIGINT);

        pid = fork();
        if (pid == 0){
            if(execle("/home/ubuntu/regexnet/build/node/bin/node", "node", "/home/ubuntu/regexnet/build/application/app.js", NULL, envp) < 0) {   
                perror("error on execl");
            }
        }
        else {
            cout << "Restart server: " << envp[1] << ", "
                 << "PID: " << pid << endl;
            fflush(stdout);
        }
    }
};

class reporter_t: public udp_client_t {
public:
    reporter_t(int report_addr, int report_port): udp_client_t(report_addr, report_port) {}

    int send_report(message_t* msg) {
        unsigned int offset = (char*)(&(msg->buffer[0])) - (char*)(&(msg->type));
        int length = udp_send((char*)&(msg->type), msg->length + offset);
        return length;
    }

} reporter(ip_str_to_int(ADDR_COLLECTOR), PORT_COLLECTOR);

class silver_bullet_t: public tcp_server_t {
//private:
//    int conn;
public:
    silver_bullet_t(int warning_addr, int warning_port): tcp_server_t(warning_addr, warning_port) {
        //conn = -1;
    }

    int get_warning() {
        //if (conn < 0 && (conn = accept_connection()) < 0)
        //    return -1;

        int conn = accept_connection();
        if (conn < 0)
            return -1;

        char id_str[32];
        while (tcp_recv(conn, id_str, 32) < 1)
            //return -1;
            ;

        int id;
        sscanf(id_str, "%d", &id);

        close(conn);
        //conn = -1;
        return id;
    }
} silver_bullet(INADDR_ANY, PORT_WARNING);

struct task_t {
    int stage; // accept conn -> 0 -> recv req -> 1 -> forward to backend -> 2 -> recv response -> forward to client -> 3
    int id;
    backend_t *backend;
    int frontend_conn;
    message_t *req;
    int backend_conn;
    message_t *res;
};

deque<task_t> task_q;
set<int> malicious_set;

int64_t get_time_us() {
    return chrono::duration_cast<chrono::microseconds>(
        chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

typedef struct {
    int id;
    int seqno;
    int64_t connection_time;
    int64_t receive_cli_time;
    int64_t request_ser_time;
    int64_t respond_ser_time;
    int64_t reply_cli_time;
} timestone;

void print_timestone(timestone ts, int64_t get_warning_time_us, int64_t complete_warning_time_us, int get_warning_seqno) {
    cout << "Timestone ## "
        << "ID: " << ts.id << ", "
        << "Sequence number: "<< ts.seqno << ", "
        << "Connection time: " << ts.connection_time << ", "
        << "Request time: " << ts.receive_cli_time << ", "
        << "Forward time: " << ts.request_ser_time << ", "
        << "Reply time: " << ts.respond_ser_time << ", "
        << "Get warning time: " << get_warning_time_us << ", "
        << "Complete warning time: " << complete_warning_time_us << ", "
        << "Get warning sequence number: " << get_warning_seqno << endl;
}

int main() {
    backend_t sandbox(ip_str_to_int(ADDR_SANDBOX), PORT_SANDBOX);
    nodejs_t nodejs[4] = {
        nodejs_t(INADDR_ANY, PORT_NODEJS_A),
        nodejs_t(INADDR_ANY, PORT_NODEJS_B),
        nodejs_t(INADDR_ANY, PORT_NODEJS_C),
        nodejs_t(INADDR_ANY, PORT_NODEJS_D)
        };
    int active_server = 0;
    int cnt=0;

    map<int, timestone> connection_life;
    map<int, int64_t> get_warning_time;
    map<int, int64_t> complete_warning_time;
    map<int, int> get_warning_seqno;

    int queue_sequence_number = 0;
    while (true) {
        ++queue_sequence_number;
        if (queue_sequence_number % 1000000 == 0)
            fprintf(stderr, "%d\n", queue_sequence_number);

        // Handle signal about malicious
        {
            while (true) {
                int malicious_id = silver_bullet.get_warning();
                if (malicious_id >= 0) {
                    fprintf(stderr, "Receive Warning, %lld\n", get_time_us() / 1000000UL);
                    get_warning_time[malicious_id] = get_time_us();
                    get_warning_seqno[malicious_id] = queue_sequence_number;
                    malicious_set.insert(malicious_id);
                    int flag = 0;
                    for (deque<task_t>::iterator itr = task_q.begin(); itr != task_q.end(); ++itr) {
                        if (malicious_set.find(itr->id) != malicious_set.end() && itr->stage == 2 && itr->backend != &sandbox)
                            ++flag;
                    }

                    if (flag > 0) {
                        for (deque<task_t>::iterator itr = task_q.begin(); itr != task_q.end(); ++itr) {
                            if (itr->stage == 2 && itr->backend != &sandbox) {
                                itr->stage = 1;
                                if (itr->backend_conn >= 0) {
                                    shutdown(itr->backend_conn, SHUT_WR);
                                    close(itr->backend_conn);
                                    itr->backend_conn = -1;
                                }
                            }
                        }

                        int next_server = (active_server + 1) % NUM_NODEJS;
                        // nodejs[active_server].restart();
                        cout << "Pretend to restart" << endl;
                        active_server = next_server;
                    }

                    complete_warning_time[malicious_id] = get_time_us();
                }
                else {
                    break;
                }
            }
        }

        // Receive connection
        {
            while (true) {
                int frontend_conn = frontend.accept_connection();
                if (frontend_conn >= 0) {
                    task_t task;
                    task.stage = 0;
                    task.frontend_conn = frontend_conn;
                    task.req = new message_t();
                    task.res = new message_t();

                    task.req->length = 0;
                    task.req->type = MESSAGE_REQUEST;

                    task_q.push_back(task);

                    connection_life[frontend_conn] = timestone();
                    connection_life[frontend_conn].connection_time = get_time_us();
                    connection_life[frontend_conn].seqno = queue_sequence_number;
                }
                else {
                    break;
                }
            }
        }

        // Receive request and forward to server / Receive response and forward to client
        if (! task_q.empty()) {
            int64_t start_time = get_time_us();
            task_t task = task_q.front();
            task_q.pop_front();
            if (task.stage == 0) {
                if (frontend.recv_request(task.frontend_conn, task.req) > 0) {
                    task.id = task.req->id;
                    task.stage = 1;
                    task.backend_conn = -1;

                    connection_life[task.frontend_conn].id = task.id;
                    connection_life[task.frontend_conn].receive_cli_time = get_time_us();
                }
                task_q.push_back(task);
            }
            else if (task.stage == 1) {
                if (malicious_set.find(task.id) == malicious_set.end()) {
                    task.backend = &nodejs[active_server];
                }
                else {
                    task.backend = &sandbox;
                }

                if (task.backend_conn < 0)
                    task.backend_conn = task.backend->request_connection();
                if (task.backend_conn >= 0) {
                    task.backend->send_request(task.backend_conn, task.req);
                    task.req->timestamp = chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now() - program_start_time).count();
                    task.stage = 2;

                    connection_life[task.frontend_conn].request_ser_time = get_time_us();
                }

                task_q.push_back(task);
            }
            else if (task.stage == 2) {
                if (task.backend->recv_response(task.backend_conn, task.res, task.id) > 0) {
                    connection_life[task.frontend_conn].respond_ser_time = get_time_us();
                    int latency = connection_life[task.frontend_conn].respond_ser_time - connection_life[task.frontend_conn].request_ser_time;
                    // fprintf(stderr, "%d\n", latency);

                    if (cnt<1000 || (task.backend != &sandbox && latency >= 500000)) {
                        cnt++;
                        // fprintf(stderr, "cnt:%d, latency:%d\n", cnt,latency);
                        reporter.send_report(task.req);
                        reporter.send_report(task.res);
                    }
                    else if (task.backend == &sandbox && latency < 500000)
                    {
                        reporter.send_report(task.req);
                        reporter.send_report(task.res);
                    }


                    int n_sent = frontend.send_response(task.frontend_conn, task.res);
                    connection_life[task.frontend_conn].reply_cli_time = get_time_us();

                    if (malicious_set.find(task.id) != malicious_set.end())
                        print_timestone(
                            connection_life[task.frontend_conn],
                            get_warning_time[task.id],
                            complete_warning_time[task.id],
                            get_warning_seqno[task.id]
                        );

                    if (shutdown(task.frontend_conn, SHUT_WR) < 0)
                        perror ("Shutdown frontend conection");
                    if (shutdown(task.backend_conn, SHUT_WR) < 0)
                        perror ("Shutdown backend conection");
                    if (close (task.frontend_conn) < 0)
                        perror ("Close frontend conection");
                    if (close (task.backend_conn) < 0)
                        perror ("Close backend conection");

                    delete task.req;
                    delete task.res;

                    malicious_set.erase(task.id);
                    get_warning_time.erase(task.id);
                    complete_warning_time.erase(task.id);
                    get_warning_seqno.erase(task.id);
                    connection_life.erase(task.frontend_conn);
                }
                else{
                    task_q.push_back(task);
                }
            }
            else {

            }

            int64_t end_time = get_time_us();
            int64_t latency_stage = end_time - start_time;
        }
    }

    return 0;
}
