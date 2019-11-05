#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "util.h"

#define TICK_NS (500 * 1000 * 1000)

int main(int argc, char *argv[])
{
    struct sockaddr_in send_addr, rr_addr;
    int send_fd, cur_fd_index;
    char *p, buf[64];
    ssize_t len;
    int *fds;
    int num_ports, num_packets, samples;
    int num_ports_mask;
    unsigned int seq = 1;
    timing_t cur_time, deadline_tick, deadline_delta_tick;
    float cpu_ghz;

    /* Parse command line arguments */

    if (argc != 5)
        goto usage_error;

    send_addr.sin_family = AF_INET;
    if (inet_aton(argv[1], &send_addr.sin_addr) == 0)
        goto usage_error;
    send_addr.sin_port = htons(strtol(argv[2], &p, 10));
    if (*argv[2] == '\0' || *p != '\0')
        goto usage_error;

    rr_addr.sin_family = AF_INET;
    rr_addr.sin_addr.s_addr = htonl(INADDR_ANY);


    num_ports = atoi(argv[3]);

    if ((num_ports != 1) && (num_ports & (num_ports - 1)))
    {
        printf("max buffers must be a power of 2\n");
        goto usage_error;
    }

    num_ports_mask = num_ports-1;

    num_packets = atoi(argv[4]);

    fds = malloc(sizeof(int*)*num_ports);

    cpu_ghz = get_cpu_ghz();
	deadline_delta_tick = to_tsc(cpu_ghz, TICK_NS);

    for(int i = 0; i < num_ports;++i) {
        send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    	if (send_fd == -1)
    	{
    		fprintf(stderr, "socket: %s\n", strerror(errno));
    		return EXIT_FAILURE;
    	}

    	rr_addr.sin_port = htons(7532 + i);
	    if (bind(send_fd, (struct sockaddr *)&rr_addr, sizeof(rr_addr)) == -1)
	    {
	        fprintf(stderr, "bind: %s\n", strerror(errno));
	        return EXIT_FAILURE;
	    }

        if (connect(send_fd, (struct sockaddr *)&send_addr, sizeof(send_addr)) == -1)
        {
            fprintf(stderr, "connect: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        fds[i] = send_fd;
    }


    /* Send data on connection */
    cur_fd_index = 0;
    samples = 0;
    do {

    	timing_start(cur_time);
    	deadline_tick = cur_time + deadline_delta_tick;
    	for(int i = 0; i < num_ports; ++i) {
    		memcpy(buf, &seq, sizeof(seq));
    		len = sizeof(seq);
    		if (send(fds[cur_fd_index], buf, len, 0) == -1)
    		{
    			fprintf(stderr, "send: %s, fd=%d\n", strerror(errno), fds[cur_fd_index]);
    			return EXIT_FAILURE;
    		}
    		++cur_fd_index;
    		cur_fd_index &= num_ports_mask;
    		++seq;
    		++samples;
    	}

		do {
			timing_start(cur_time);
		} while ( cur_time < deadline_tick);

    } while( samples < num_packets);

    for(int i = 0; i < num_ports;++i) {
        close(fds[i]);
    }

    free(fds);

    return 0;

usage_error:
    fprintf(stderr, "Usage: %s <addr> <port> num_ports num_packets\n", argv[0]);
    printf("    num_ports: maximum number of source ports to use (must be power of 2)\n");
	printf("    num_packets: total number of packets to transmit\n");
    return EXIT_FAILURE;
}
