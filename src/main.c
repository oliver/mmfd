#include "mmfd.h"
#include "babeld.h"
#include "error.h"
#include "util.h"
#include "neighbour.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if_tun.h>
#include <linux/ipv6.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>

#define MTU 1280
#define BABELD_RECONNECT_TIMEOUT 90

static void change_fd(int efd, int fd, int type, uint32_t events);
static void handle_udp_packet(struct context *ctx,  struct sockaddr_in6 *src_addr, struct header *hdr, uint8_t *packet, ssize_t len);
struct context ctx = {};

#define FMT_NONCE "0x%08x"

void settimer(int sec, int *fd) {
	struct itimerspec ts;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	ts.it_value.tv_sec = sec;
	ts.it_value.tv_nsec = 0;

	if (timerfd_settime(*fd, 0, &ts, NULL) < 0) {
		perror("could not set timer interval\n");
		close(*fd);
		exit_error("could not set timer, exiting");
	}
}

int udp_open() {
	int fd = socket(PF_INET6, SOCK_DGRAM | SOCK_NONBLOCK, 0);

	if (fd < 0)
		exit_error("creating socket");

	if (ctx.bind) {
		for (int i=0;i<VECTOR_LEN(ctx.interfaces);i++) {
			if(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, VECTOR_INDEX(ctx.interfaces, i), strnlen(VECTOR_INDEX(ctx.interfaces, i), IFNAMSIZ-1))) {
				exit_error("error on setsockopt");
			}
		}
	}

	struct sockaddr_in6 server_addr = {};

	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_addr = in6addr_any;
	server_addr.sin6_port = htons(PORT);

	if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
		exit_errno("bind failed");

	return fd;
}

/**
 * tun_open - open a tun device, set mtu and return it
 * @ifname: name of the interface to open
 * @mtu: mtu to assign to the device
 * @dev_name: path to the tun device node (normally this should be "/dev/net/tun")
 *
 * Return: filedescriptor to tun device on success, otherwise -1
 */
int tun_open(const char *ifname, uint16_t mtu, const char *dev_name) {
	int ctl_sock = -1;
	struct ifreq ifr = {};

	// open tun iface
	int fd = open(dev_name, O_RDWR|O_NONBLOCK);
	if (fd < 0)
		exit_errno("could not open TUN/TAP device file");

	// set name of the iface
	if (ifname)
		strncpy(ifr.ifr_name, ifname, IFNAMSIZ-1);

	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		puts("unable to open TUN/TAP interface: TUNSETIFF ioctl failed");
		goto error;
	}

	// open control socket to set the mtu of the iface
	ctl_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (ctl_sock < 0)
		exit_errno("socket");

	if (ioctl(ctl_sock, SIOCGIFMTU, &ifr) < 0)
		exit_errno("SIOCGIFMTU ioctl failed");

	if (ifr.ifr_mtu != mtu) {
		ifr.ifr_mtu = mtu;
		if (ioctl(ctl_sock, SIOCSIFMTU, &ifr) < 0) {
			puts("unable to set TUN/TAP interface MTU: SIOCSIFMTU ioctl failed");
			goto error;
		}
	}

	ifr.ifr_flags = IFF_UP | IFF_RUNNING| IFF_MULTICAST | IFF_NOARP | IFF_POINTOPOINT;
	if (ioctl(ctl_sock, SIOCSIFFLAGS, &ifr) < 0 ) {
		puts("unable to set TUN/TAP interface UP: SIOCSIFFLAGS ioctl failed");
		goto error;
	}

	if (close(ctl_sock))
		puts("close");

	return fd;

error:
	if (ctl_sock >= 0) {
		if (close(ctl_sock))
			puts("close");
	}

	close(fd);
	return -1;
}

bool forward_packet(struct context *ctx, uint8_t *packet, ssize_t len, uint32_t nonce, struct sockaddr_in6 *src_addr) {
	for (int i = 0; i < VECTOR_LEN(ctx->seen); i++) {
		if (VECTOR_INDEX(ctx->seen, i) == nonce) {
			log_verbose(ctx, "Dropped packet with already seen nonce " FMT_NONCE "\n", nonce);
			return false;
		}
	}

	while (VECTOR_LEN(ctx->seen) > 2000)
		VECTOR_DELETE(ctx->seen, 0);

	VECTOR_ADD(ctx->seen, nonce);

	struct header hdr = {
		.nonce = nonce,
	};

	struct iovec iov[2] = {
		{
			.iov_base = &hdr,
			.iov_len = sizeof(hdr),
		},
		{
			.iov_base = packet,
			.iov_len = len,
		}
	};

	// prepare some log output, if necessary
	char dest_ip_str[INET6_ADDRSTRLEN] = {};
	char src_ip_str[INET6_ADDRSTRLEN] = {};
	if (ctx->verbose) {
		struct ipv6hdr *hdr = (struct ipv6hdr*)packet;
		inet_ntop(AF_INET6, &hdr->daddr, dest_ip_str, INET6_ADDRSTRLEN);
		src_ip_str[0]='\0';
		if (src_addr)
			inet_ntop(AF_INET6, &(src_addr->sin6_addr), src_ip_str, INET6_ADDRSTRLEN);
	}

	if (VECTOR_LEN(ctx->neighbours) == 0) {
		log_verbose(ctx, "No neighbour found. Cannot forward packet with destaddr=%s, nonce=" FMT_NONCE ".\n", dest_ip_str, nonce);
		return true;
	}

	for (int i = 0; i < VECTOR_LEN(ctx->neighbours); i++) {
		struct neighbour *neighbour = &VECTOR_INDEX(ctx->neighbours, i);

		int forwardmessage=1;
		if (src_addr) {
			forwardmessage = memcmp(src_addr,&(neighbour->address),sizeof(neighbour->address));
		}

		if (forwardmessage) {
			struct msghdr msg = {
				.msg_name = &neighbour->address,
				.msg_namelen = sizeof(struct sockaddr_in6),
				.msg_iov = iov,
				.msg_iovlen = 2,
			};

			if (ctx->verbose) {
				// convert information about the neigh to strings
				char neigh_ip_str[INET6_ADDRSTRLEN] = {};
				char neigh_ifname[IFNAMSIZ] = {};
				inet_ntop(AF_INET6, &neighbour->address.sin6_addr, neigh_ip_str, INET6_ADDRSTRLEN);
				if_indextoname(neighbour->address.sin6_scope_id, neigh_ifname);

				log_verbose(ctx, "Forwarding packet from %s with destaddr=%s, "
						"nonce=" FMT_NONCE " to %s%%%s.\n",
						src_ip_str, dest_ip_str, nonce, neigh_ip_str, neigh_ifname);
			}
			sendmsg(ctx->udpfd, &msg, 0);
		}
	}
	return true;
}

void udp_handle_in(struct context *ctx, int fd) {
	while (1) {
		struct header hdr;
		uint8_t buffer[1500];
		struct sockaddr_storage src_addr;

		struct iovec iov[2] = {
			{
				.iov_base = &hdr,
				.iov_len = sizeof(hdr),
			},
			{
				.iov_base = buffer,
				.iov_len = sizeof(buffer),
			}
		};

		struct msghdr message = {
			.msg_name = &src_addr,
			.msg_namelen = sizeof(src_addr),
			.msg_iov = iov,
			.msg_iovlen = 2,
			.msg_control = 0,
			.msg_controllen = 0,
		};

		ssize_t count = recvmsg(fd, &message, 0);

		if (count == -1 && errno == EAGAIN)
			break;

		if (count <= sizeof(hdr))
			continue;

		if (count == -1)
			perror("Error during recvmsg");
		else if (message.msg_flags & MSG_TRUNC)
			printf("Message too long for buffer\n");
		else
			handle_udp_packet(ctx, (struct sockaddr_in6 *)&src_addr, &hdr, buffer, count - sizeof(hdr));
	}
}

void handle_udp_packet(struct context *ctx,  struct sockaddr_in6 *src_addr, struct header *hdr, uint8_t *packet, ssize_t len) {
	if (forward_packet(ctx, packet, len, hdr->nonce, src_addr)) {
		log_verbose(ctx, "writing packet to tun interface\n");
		write(ctx->tunfd, packet, len);
	}
}

void handle_packet(struct context *ctx, uint8_t *packet, ssize_t len) {
	uint32_t nonce = rand();

	forward_packet(ctx, packet, len, nonce, NULL);
}

void tun_handle_in(struct context *ctx, int fd) {
	ssize_t count;

	uint8_t buf[MTU];

	while (1) {
		count = read(fd, buf, MTU);

		if (count == -1) {
			/* If errno == EAGAIN, that means we have read all
			   data. So go back to the main loop. */
			if (errno != EAGAIN) {
				perror("read");
			}
			break;
		} else if (count == 0) {
			break;
		}

		if (count < 40) // ipv6 header has 40 bytes
			continue;

		struct ipv6hdr *hdr = (struct ipv6hdr*)buf;

		if (hdr->version != 6) {
			log_verbose(ctx, "Dropping non-IPv6 packet.\n");
			continue;
		}

		// Ignore any non-multicast packets
		if (hdr->daddr.s6_addr[0] != 0xff) {
			if (ctx->verbose) {
				char dest_ip_str[INET6_ADDRSTRLEN];
				inet_ntop(AF_INET6, &hdr->daddr, dest_ip_str, INET6_ADDRSTRLEN);
				log_verbose(ctx, "Dropping non multicast packet destined to %s.\n", dest_ip_str);
			}

			continue;
		}

		handle_packet(ctx, buf, count);
	}
}

void reconnect_babeld(struct context *ctx) {
	struct itimerspec delay = {};
	log_debug(ctx, "reconnect_babeld - starting reconnect timer\n");

	if (ctx->babelfd) {
		change_fd(ctx->efd, ctx->babelfd, EPOLL_CTL_DEL, EPOLLIN);
		close(ctx->babelfd);
		log_debug(ctx, "closing babeld connection\n");

		delay = (struct itimerspec) {
			.it_value = {
				.tv_sec = 1,
				.tv_nsec = 0,
			}
		};
	} else {
		log_debug(ctx, "babelfd was 0 - just starting reconnect\n");
		delay = (struct itimerspec) {
			.it_value = {
				.tv_sec = 0,
				.tv_nsec = 1,
			}
		};
	}

	ctx->babelfd = 0;

	timerfd_settime(ctx->babeld_reconnect_tfd, 0, &delay, NULL);
}

void change_fd(int efd, int fd, int type, uint32_t events) {
	struct epoll_event event = {};
	event.data.fd = fd;
	event.events = events;

	int s = epoll_ctl(efd, type, fd, &event);
	if (s == -1)
		exit_error("epoll_ctl");
}

void loop(struct context *ctx) {
	ctx->efd = epoll_create(1);

	if (ctx->efd == -1)
		exit_errno("epoll_create");

	change_fd(ctx->efd, ctx->udpfd, EPOLL_CTL_ADD, EPOLLIN | EPOLLET);
	change_fd(ctx->efd, ctx->tunfd, EPOLL_CTL_ADD, EPOLLIN | EPOLLET);
	change_fd(ctx->efd, ctx->babeld_reconnect_tfd, EPOLL_CTL_ADD, EPOLLIN);

	if (ctx->timerfd > 0)
		change_fd(ctx->efd, ctx->timerfd, EPOLL_CTL_ADD, EPOLLIN);

	int maxevents = 64;
	struct epoll_event *events;
	events = calloc(maxevents, sizeof(struct epoll_event));

	while (1) {
		log_debug(ctx, "epoll_wait: ... ");
		int n = epoll_wait(ctx->efd, events, maxevents, -1);
		log_debug(ctx, "%i\n", n);

		for ( int i = 0; i < n; i++ ) {
			if (ctx->udpfd == events[i].data.fd) {
				log_debug(ctx, "event on udpfd\n");
				if (events[i].events & EPOLLIN)
					udp_handle_in(ctx, events[i].data.fd);
			} else if (ctx->timerfd >0 && ctx->timerfd == events[i].data.fd) {
				uint64_t res;
				int n = read(ctx->timerfd, &res, sizeof(res));
				log_debug(ctx, "neighbour-timer expired: read() returned %d, res=%li\n", n, res);
				print_neighbours(ctx);
			} else if (ctx->tunfd == events[i].data.fd) {
				log_debug(ctx, "event on tunfd\n");
				if (events[i].events & EPOLLIN)
					tun_handle_in(ctx, events[i].data.fd);
			} else if (ctx->babeld_reconnect_tfd == events[i].data.fd) {
				log_debug(ctx, "event on babeld_reconnect_tfd\n");

				if (events[i].events & EPOLLIN) {
					settimer(0, &ctx->babeld_reconnect_tfd); // disarm reconnect timer
					unsigned long long nEvents;
					read(ctx->babeld_reconnect_tfd, &nEvents, sizeof(nEvents));

					log_debug(ctx, "Re-Connecting to babeld after timer on %i fired by closing and re-opening babeld socket.\n", ctx->babeld_reconnect_tfd);

					if (ctx->babelfd) {
						change_fd(ctx->efd, ctx->babelfd, EPOLL_CTL_DEL, EPOLLIN);
						close(ctx->babelfd);
					}

					flush_neighbours(ctx);
					if (ctx->babeld_buffer != NULL)
						free(ctx->babeld_buffer);

					ctx->babeld_buffer = NULL;

					ctx->babelfd = babeld_connect(ctx);

					settimer(BABELD_RECONNECT_TIMEOUT, &ctx->babeld_reconnect_tfd); // reset reconnect timer

					change_fd(ctx->efd, ctx->babelfd, EPOLL_CTL_ADD, EPOLLIN);
					log_verbose(ctx, "reconnected to babeld after timer-event fired.\n");
				}
			} else if (ctx->babelfd == events[i].data.fd) {
				log_debug(ctx, "event on babelfd\n");
				settimer(BABELD_RECONNECT_TIMEOUT, &ctx->babeld_reconnect_tfd); // reset reconnect timer
				if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
					printf("some error on babelfd happened or HUP\n");
					reconnect_babeld(ctx);
				} else if (events[i].events & EPOLLIN) {
					int babelhandle_status = babeld_handle_in(ctx, events[i].data.fd);
					if ( babelhandle_status < 0 ) {
						printf("babeld_handle_in was not successful - reconnecting\n");
						reconnect_babeld(ctx);
					} else if ( babelhandle_status == 0 ) {
						log_debug(ctx, "waiting for more data to appear on babel socket.\n");
					} else if ( babelhandle_status > 0 )  {
						log_debug(ctx, "received ok. -- waiting for more data to appear on babel socket\n");
					}
				}
			} else {
				char junk;
				read(events[i].data.fd, &junk, 1);
				fprintf(stderr, "THIS SHOULD NEVER HAPPEN: Data arrived on fd %d which we are not monitoring in our loop, discarding data: %c\n", events[i].data.fd, junk);
			}
		}
	}

	free(events);
}

void usage() {
	puts("Usage: mmfd [-h] [-v] [-d] [-D <devicename>] [-p <port>] [-i <mesh-device>] [-i <mesh-device>]");
	puts("  -v     verbose");
	puts("  -d     debug");
	puts("  -D     name of the mmfd device");
	puts("  -p     port of the babeld-socket, default: 33123");
	puts("  -i     bind to interface, may be specified multiple times");
	puts("  -h     this help");
}

int main(int argc, char *argv[]) {
	int c;
	ctx.babelport = 33123;
	char mmfd_device[IFNAMSIZ] = "mmfd0";
	ctx.bind = false;
	ctx.verbose = false;
	ctx.debug = false;


	while ((c = getopt(argc, argv, "vhdp:D:i:")) != -1)
		switch (c) {
			case 'd':
				ctx.debug = true;
				break;
			case 'p':
				ctx.babelport = atoi(optarg);
				break;
			case 'v':
				ctx.verbose = true;
				break;
			case 'h':
				usage();
				exit(EXIT_SUCCESS);
			case 'D':
				snprintf(mmfd_device, IFNAMSIZ, "%s", optarg);
				break;
			case 'i':
				if (if_nametoindex(optarg)) {
					VECTOR_ADD(ctx.interfaces, optarg);
					ctx.bind=true;
				}
				else {
					fprintf(stderr, "Could not find device %s. ignoring.\n", optarg);
				}
				break;
			default:
				fprintf(stderr, "Invalid parameter %c ignored.\n", c);
		}

	int rfd = open("/dev/urandom", O_RDONLY);
	unsigned int seed;
	read(rfd, &seed, sizeof(seed));
	close(rfd);
	srand(seed);

	ctx.udpfd = udp_open();
	ctx.tunfd = tun_open(mmfd_device, MTU, "/dev/net/tun");

	if (ctx.tunfd == -1)
		exit_error("Can not create tun device");

	ctx.babeld_reconnect_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

	reconnect_babeld(&ctx);

	ctx.timerfd=-1;

	if (ctx.verbose) { // the neighbour-timer is used exclusively to print the neighbour list in verbose modes. No need to initialize it when not verbose.
		printf("arming neighbour-timer\n");
		ctx.timerfd = timerfd_create(CLOCK_MONOTONIC, 0);
		if (ctx.timerfd == -1) {
			printf("timerfd_create() failed: errno=%d\n", errno);
			return EXIT_FAILURE;
		}
		struct itimerspec ts;
		ts.it_interval.tv_sec = 15;
		ts.it_interval.tv_nsec = 0;
		ts.it_value.tv_sec = 15;
		ts.it_value.tv_nsec = 0;

		if (timerfd_settime(ctx.timerfd, 0, &ts, NULL) < 0) {
			perror("could not set timer interval\n");
			close(ctx.timerfd);
			exit_error("could not set timer, exiting");
		}

	}

	loop(&ctx);
}
