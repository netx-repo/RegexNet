#include <arpa/inet.h>

int ip_str_to_int(const char addr_str[32]) {
    int addr_int;
	inet_pton(AF_INET, addr_str, &addr_int);
    return addr_int;
}