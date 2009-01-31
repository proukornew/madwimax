/*
 * This is a proof-of-concept driver for Samsung SWC-U200 wimax dongle.
 * Copyright (C) 2008 Alexander Gordeev <lasaine@lvk.cs.msu.su>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <libusb-1.0/libusb.h>

#include "config.h"
#include "logging.h"
#include "protocol.h"
#include "wimax.h"
#include "tap_dev.h"


/* variables for the command-line parameters */
static int daemonize = 0;
static int diode_on = 1;
static int detach_dvd = 0;

#define MATCH_BY_LIST		0
#define MATCH_BY_VID_PID	1
#define MATCH_BY_BUS_DEV	2

static int match_method = MATCH_BY_LIST;

/* for matching by list... */
typedef struct usb_device_id_t {
	int vendorID;
	int productID;
} usb_device_id_t;

/* list of all known devices */
static usb_device_id_t wimax_dev_ids[] = {
	{ 0x04e8, 0x6761 },
	{ 0x04e9, 0x6761 },
};

/* for other methods of matching... */
static union {
	struct {
		unsigned int vid;
		unsigned int pid;
	};
	struct {
		unsigned int bus;
		unsigned int dev;
	};
} match_params;

/* USB-related parameters */
#define IF_MODEM		0
#define IF_DVD			1

#define EP_IN			(2 | LIBUSB_ENDPOINT_IN)
#define EP_OUT			(4 | LIBUSB_ENDPOINT_OUT)

#define MAX_PACKET_LEN		0x4000

/* information collector */
static struct wimax_dev_status wd_status;

char *wimax_states[] = {"INIT", "SYNC", "NEGO", "NORMAL", "SLEEP", "IDLE", "HHO", "FBSS", "RESET", "RESERVED", "UNDEFINED", "BE", "NRTPS", "RTPS", "ERTPS", "UGS", "INITIAL_RNG", "BASIC", "PRIMARY", "SECONDARY", "MULTICAST", "NORMAL_MULTICAST", "SLEEP_MULTICAST", "IDLE_MULTICAST", "FRAG_BROADCAST", "BROADCAST", "MANAGEMENT", "TRANSPORT"};

/* libusb stuff */
static struct libusb_context *ctx = NULL;
static struct libusb_device_handle *devh = NULL;
static struct libusb_transfer *req_transfer = NULL;

static unsigned char read_buffer[MAX_PACKET_LEN];

static int tap_fd = -1;
static char tap_dev[20];

static nfds_t nfds;
static struct pollfd* fds = NULL;

static int first_nego_flag = 0;
static int device_disconnected = 0;


#define CHECK_NEGATIVE(x) {if((r = (x)) < 0) return r;}
#define CHECK_DISCONNECTED(x) {if((r = (x)) == LIBUSB_ERROR_NO_DEVICE) exit_release_resources(0);}

static void exit_release_resources(int code);

static struct libusb_device_handle* find_wimax_device(void)
{
	struct libusb_device **devs;
	struct libusb_device *found = NULL;
	struct libusb_device *dev;
	struct libusb_device_handle *handle = NULL;
	int i = 0;
	int r;

	if (libusb_get_device_list(ctx, &devs) < 0)
		return NULL;

	while (!found && (dev = devs[i++]) != NULL) {
		struct libusb_device_descriptor desc;
		unsigned int j = 0;

		r = libusb_get_device_descriptor(dev, &desc);
		if (r < 0) {
			continue;
		}
		switch (match_method) {
			case MATCH_BY_LIST: {
				for (j = 0; j < sizeof(wimax_dev_ids); j++) {
					if (desc.idVendor == wimax_dev_ids[j].vendorID && desc.idProduct == wimax_dev_ids[j].productID) {
						found = dev;
						break;
					}
				}
				break;
			}
			case MATCH_BY_VID_PID: {
				if (desc.idVendor == match_params.vid && desc.idProduct == match_params.pid) {
					found = dev;
				}
				break;
			}
			case MATCH_BY_BUS_DEV: {
				if (libusb_get_bus_number(dev) == match_params.bus && libusb_get_device_address(dev) == match_params.dev) {
					found = dev;
				}
				break;
			}
		}
	}

	if (found) {
		r = libusb_open(found, &handle);
		if (r < 0)
			handle = NULL;
	}

	libusb_free_device_list(devs, 1);
	return handle;
}

static int set_data(unsigned char* data, int size)
{
	int r;
	int transferred;

	debug_dumphexasc(1, "Bulk write:", data, size);

	r = libusb_bulk_transfer(devh, EP_OUT, data, size, &transferred, 0);
	if (r < 0) {
		debug_msg(0, "bulk write error %d", r);
		if (r == LIBUSB_ERROR_NO_DEVICE) {
			exit_release_resources(0);
		}
		return r;
	}
	if (transferred < size) {
		debug_msg(0, "short write (%d)", r);
		return -1;
	}
	return r;
}

static void cb_req(struct libusb_transfer *transfer)
{
	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		debug_msg(0, "async bulk read error %d", transfer->status);
		if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
			device_disconnected = 1;
			return;
		}
	} else {
		debug_dumphexasc(1, "Async read:", transfer->buffer, transfer->actual_length);
		process_response(&wd_status, transfer->buffer, transfer->actual_length);
	}
	if (libusb_submit_transfer(req_transfer) < 0) {
		debug_msg(0, "async read transfer sumbit failed");
	}
}

/* get link_status */
int get_link_status()
{
	return wd_status.link_status;
}

/* run specified script */
static int raise_event(char *event)
{
	int pid = fork();

	if(pid < 0) { /* error */
		return -1;
	} else if (pid > 0) { /* parent */
		return pid;
	} else { /* child */
		char *args[] = {SYSCONFDIR "/event.sh", event, tap_dev, NULL};
		char *env[1] = {NULL};
		/* run the program */
		execve(args[0], args, env);
		return -1;
	}
}

/* brings interface up and runs a user-supplied script */
static int if_up()
{
	tap_bring_up(tap_fd, tap_dev);
	debug_msg(0, "Starting if-up script...");
	raise_event("if-up");
	return 0;
}

/* brings interface down and runs a user-supplied script */
static int if_down()
{
	debug_msg(0, "Starting if-down script...");
	raise_event("if-down");
	tap_bring_down(tap_fd, tap_dev);
	return 0;
}

/* set link_status */
void set_link_status(int link_status)
{
	wd_status.info_updated |= WDS_LINK_STATUS;

	if (wd_status.link_status == link_status) return;

	if (wd_status.link_status < 2 && link_status == 2) {
		if_up();
	}
	if (wd_status.link_status == 2 && link_status < 2) {
		if_down();
	}
	if (link_status == 1) {
		first_nego_flag = 1;
	}

	wd_status.link_status = link_status;
}

/* get state */
int get_state()
{
	return wd_status.state;
}

/* set state */
void set_state(int state)
{
	wd_status.state = state;
	wd_status.info_updated |= WDS_STATE;
	if (state >= 1 && state <= 3 && wd_status.link_status != (state - 1)) {
		set_link_status(state - 1);
	}
}

static int alloc_transfers(void)
{
	req_transfer = libusb_alloc_transfer(0);
	if (!req_transfer)
		return -ENOMEM;

	libusb_fill_bulk_transfer(req_transfer, devh, EP_IN, read_buffer,
		sizeof(read_buffer), cb_req, NULL, 0);

	return 0;
}

int write_netif(const void *buf, int count)
{
	return tap_write(tap_fd, buf, count);
}

static int read_tap()
{
	unsigned char buf[MAX_PACKET_LEN];
	int hlen = get_D_header_len();
	int r;
	int len;

	r = tap_read(tap_fd, buf + hlen, MAX_PACKET_LEN - hlen);

	if (r < 0)
	{
		debug_msg(0, "Error while reading from TAP interface");
		return r;
	}

	if (r == 0)
	{
		return 0;
	}

	len = fill_outgoing_packet_header(buf, MAX_PACKET_LEN, r);
	debug_dumphexasc(1, "Outgoing packet:", buf, len);
	r = set_data(buf, len);

	return r;
}

static int process_events_once(int timeout)
{
	struct timeval tv;
	int r;
	int libusb_delay;
	int delay;
	unsigned int i;
	char process_libusb = 0;

	r = libusb_get_next_timeout(ctx, &tv);
	if (r == 1 && tv.tv_sec == 0 && tv.tv_usec == 0)
	{
		r = libusb_handle_events_timeout(ctx, &tv);
	}

	delay = libusb_delay = tv.tv_sec * 1000 + tv.tv_usec;
	if (delay == 0 || delay > timeout)
	{
		delay = timeout;
	}

	CHECK_NEGATIVE(poll(fds, nfds, delay));

	process_libusb = (r == 0 && delay == libusb_delay);

	for (i = 0; i < nfds; ++i)
	{
		if (fds[i].fd == tap_fd) {
			if (fds[i].revents)
			{
				CHECK_NEGATIVE(read_tap());
			}
			continue;
		}
		process_libusb |= fds[i].revents;
	}

	if (process_libusb)
	{
		struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
		CHECK_NEGATIVE(libusb_handle_events_timeout(ctx, &tv));
	}

	return 0;
}

/* handle events until timeout is reached or all of the events in event_mask happen */
static int process_events_by_mask(int timeout, unsigned int event_mask)
{
	struct timeval start, curr;
	int r;
	int delay = timeout;

	CHECK_NEGATIVE(gettimeofday(&start, NULL));

	while ((event_mask == 0 || (wd_status.info_updated & event_mask) != event_mask) && delay >= 0) {
		long a;

		CHECK_NEGATIVE(process_events_once(delay));

		if (device_disconnected) {
			exit_release_resources(0);
		}

		CHECK_NEGATIVE(gettimeofday(&curr, NULL));

		a = (curr.tv_sec - start.tv_sec) * 1000 + (curr.tv_usec - start.tv_usec) / 1000;
		delay = timeout - a;
	}

	wd_status.info_updated &= ~event_mask;

	return (delay > 0) ? delay : 0;
}

/* set close-on-exec flag on the file descriptor */
int set_coe(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFD);
	if (flags == -1)
	{
		debug_msg(0, "failed to set close-on-exec flag on fd %d", fd);
		return -1;
	}
	flags |= FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, flags) == -1)
	{
		debug_msg(0, "failed to set close-on-exec flag on fd %d", fd);
		return -1;
	}

	return 0;
}

int alloc_fds()
{
	int i;
	const struct libusb_pollfd **usb_fds = libusb_get_pollfds(ctx);

	if (!usb_fds)
	{
		return -1;
	}

	nfds = 0;
	while (usb_fds[nfds])
	{
		nfds++;
	}
	if (tap_fd != -1) {
		nfds++;
	}

	if(fds != NULL) {
		free(fds);
	}

	fds = (struct pollfd*)calloc(nfds, sizeof(struct pollfd));
	for (i = 0; usb_fds[i]; ++i)
	{
		fds[i].fd = usb_fds[i]->fd;
		fds[i].events = usb_fds[i]->events;
		set_coe(usb_fds[i]->fd);
	}
	if (tap_fd != -1) {
		fds[i].fd = tap_fd;
		fds[i].events = POLLIN;
		fds[i].revents = 0;
	}

	free(usb_fds);

	return 0;
}

void cb_add_pollfd(int fd, short events, void *user_data)
{
	alloc_fds();
}

void cb_remove_pollfd(int fd, void *user_data)
{
	alloc_fds();
}

static int init(void)
{
	unsigned char data2[] = {0x57, 0x50, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned char data3[] = {0x57, 0x43, 0x12, 0x00, 0x15, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x04, 0x50, 0x04, 0x00, 0x00};
	unsigned char req_data[MAX_PACKET_LEN];
	int len;
	int r;

	alloc_transfers();

	debug_msg(0, "Continuous async read start...");
	CHECK_DISCONNECTED(libusb_submit_transfer(req_transfer));

	len = fill_protocol_info_req(req_data, MAX_PACKET_LEN,
			USB_HOST_SUPPORT_SELECTIVE_SUSPEND | USB_HOST_SUPPORT_DL_SIX_BYTES_HEADER |
			USB_HOST_SUPPORT_UL_SIX_BYTES_HEADER | USB_HOST_SUPPORT_DL_MULTI_PACKETS);
	set_data(req_data, len);

	process_events_by_mask(500, WDS_PROTO_FLAGS);

	set_data(data2, sizeof(data2));

	process_events_by_mask(500, WDS_OTHER);

	set_data(data3, sizeof(data3));

	len = fill_string_info_req(req_data, MAX_PACKET_LEN);
	set_data(req_data, len);

	process_events_by_mask(500, WDS_CHIP | WDS_FIRMWARE);

	debug_msg(0, "Chip info: %s", wd_status.chip);
	debug_msg(0, "Firmware info: %s", wd_status.firmware);

	len = fill_diode_control_cmd(req_data, MAX_PACKET_LEN, diode_on);
	set_data(req_data, len);

	len = fill_mac_req(req_data, MAX_PACKET_LEN);
	set_data(req_data, len);

	process_events_by_mask(500, WDS_MAC);

	debug_msg(0, "MAC: %02x:%02x:%02x:%02x:%02x:%02x", wd_status.mac[0], wd_status.mac[1], wd_status.mac[2], wd_status.mac[3], wd_status.mac[4], wd_status.mac[5]);

	len = fill_string_info_req(req_data, MAX_PACKET_LEN);
	set_data(req_data, len);

	process_events_by_mask(500, WDS_CHIP | WDS_FIRMWARE);

	len = fill_auth_policy_req(req_data, MAX_PACKET_LEN);
	set_data(req_data, len);

	process_events_by_mask(500, WDS_OTHER);

	len = fill_auth_method_req(req_data, MAX_PACKET_LEN);
	set_data(req_data, len);

	process_events_by_mask(500, WDS_OTHER);

	len = fill_auth_set_cmd(req_data, MAX_PACKET_LEN);
	set_data(req_data, len);

	return 0;
}

static int scan_loop(void)
{
	unsigned char req_data[MAX_PACKET_LEN];
	int len;

	while (1)
	{
		if (wd_status.link_status == 0) {
			len = fill_find_network_req(req_data, MAX_PACKET_LEN, 1);
			set_data(req_data, len);

			process_events_by_mask(5000, WDS_LINK_STATUS);

			if (wd_status.link_status == 0) {
				debug_msg(0, "Network not found.");
			} else {
				debug_msg(0, "Network found.");
			}
		} else {
			//len = fill_connection_params1_req(req_data, MAX_PACKET_LEN);
			//r = set_data(req_data, len);
			//if (r < 0) {
			//	return r;
			//}

			len = fill_connection_params2_req(req_data, MAX_PACKET_LEN);
			set_data(req_data, len);

			process_events_by_mask(500, WDS_RSSI | WDS_CINR | WDS_TXPWR | WDS_FREQ | WDS_BSID);

			debug_msg(0, "RSSI: %d   CINR: %f   TX Power: %d   Frequency: %d", wd_status.rssi, wd_status.cinr, wd_status.txpwr, wd_status.freq);
			debug_msg(0, "BSID: %02x:%02x:%02x:%02x:%02x:%02x", wd_status.bsid[0], wd_status.bsid[1], wd_status.bsid[2], wd_status.bsid[3], wd_status.bsid[4], wd_status.bsid[5]);

			len = fill_state_req(req_data, MAX_PACKET_LEN);
			set_data(req_data, len);

			process_events_by_mask(500, WDS_STATE);

			debug_msg(0, "State: %s   Number: %d   Response: %d", wimax_states[wd_status.state], wd_status.state, wd_status.link_status);

			if (first_nego_flag) {
				first_nego_flag = 0;
				len = fill_find_network_req(req_data, MAX_PACKET_LEN, 2);
				set_data(req_data, len);
			}

			process_events_by_mask(5000, WDS_LINK_STATUS);
		}
	}

	return 0;
}

static void parse_args(int argc, char **argv)
{
	while (1)
	{
		int c;
		/* getopt_long stores the option index here. */
		int option_index = 0;
		static struct option long_options[] =
		{
			{"verbose",		no_argument,		0, 'v'},
			{"quiet",		no_argument,		0, 'q'},
			{"daemonize",		no_argument,		0, 'd'},
			{"diode-off",		no_argument,		0, 'o'},
			{"detach-dvd",		no_argument,		0, 'f'},
			{"device",		required_argument,	0, 1},
			{"exact-device",	required_argument,	0, 2},
			{"version",		no_argument,		0, 'V'},
			{"help",		no_argument,		0, 'h'},
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "vqdofVh", long_options, &option_index);

		/* detect the end of the options. */
		if (c == -1)
			break;

		switch (c)
		{
			case 'v': {
					inc_debug_level();
					break;
				}
			case 'q': {
					set_debug_level(-1);
					break;
				}
			case 'd': {
					daemonize = 1;
					break;
				}
			case 'o': {
					diode_on = 0;
					break;
				}
			case 'f': {
					detach_dvd = 1;
					break;
				}
			case 'V': {
					version();
					exit(0);
					break;
				}
			case 'h': {
					usage(argv[0]);
					exit(0);
					break;
				}
			case 1: {
					char *delim = strchr(optarg, ':');

					if (delim != NULL) {
						unsigned long int vid, pid;
						char *c1, *c2;

						*delim = 0;

						vid = strtoul(optarg, &c1, 16);
						pid = strtoul(delim + 1, &c2, 16);
						if (!*c1 && !*c2 && vid < 0x10000 && pid < 0x10000) {
							match_method = MATCH_BY_VID_PID;
							match_params.vid = vid;
							match_params.pid = pid;
							break;
						}
					}

					fprintf(stderr, "Error parsing VID:PID combination.\n");
					exit(1);
					break;
				}
			case 2: {
					char *delim = strchr(optarg, '/');

					if (delim != NULL) {
						unsigned long int bus, dev;
						char *c1, *c2;

						*delim = 0;

						bus = strtoul(optarg, &c1, 10);
						dev = strtoul(delim + 1, &c2, 10);
						if (!*c1 && !*c2) {
							match_method = MATCH_BY_BUS_DEV;
							match_params.bus = bus;
							match_params.dev = dev;
							break;
						}
					}

					fprintf(stderr, "Error parsing BUS/DEV combination.\n");
					exit(1);
					break;
				}
			case '?': {
					/* getopt_long already printed an error message. */
					usage(argv[0]);
					exit(1);
					break;
				}
			default: {
					exit(1);
				}
		}
	}
}

static void exit_close_usb(int code);

static void exit_release_resources(int code)
{
	if(req_transfer != NULL) {
		libusb_cancel_transfer(req_transfer);
		libusb_free_transfer(req_transfer);
	}
	if(tap_fd >= 0) {
		if_down();
		while (wait(NULL) > 0) {}
		tap_close(tap_fd, tap_dev);
	}
	libusb_set_pollfd_notifiers(ctx, NULL, NULL, NULL);
	if(fds != NULL) {
		free(fds);
	}
	libusb_release_interface(devh, 0);
	exit_close_usb(code);
}

static void exit_close_usb(int code)
{
	libusb_unlock_events(ctx);
	libusb_close(devh);
	libusb_exit(ctx);
	exit(code);
}

static void sighandler_exit(int signum) {
	exit_release_resources(0);
}

static void sighandler_wait_child(int signum) {
	int status;
	wait3(&status, WNOHANG, NULL);
	debug_msg(0, "Child exited with status %d", status);
}

int main(int argc, char **argv)
{
	struct sigaction sigact;
	int r = 1;

	parse_args(argc, argv);

	r = libusb_init(&ctx);
	if (r < 0) {
		debug_msg(0, "failed to initialise libusb");
		exit(1);
	}

	devh = find_wimax_device();
	if (devh == NULL) {
		debug_msg(0, "Could not find/open device");
		exit_close_usb(1);
	}

	if (detach_dvd && libusb_kernel_driver_active(devh, IF_DVD) == 1) {
		r = libusb_detach_kernel_driver(devh, IF_DVD);
		if (r < 0) {
			debug_msg(0, "kernel driver detach error %d", r);
		} else {
			debug_msg(0, "detached pseudo-DVD kernel driver");
		}
	}

	r = libusb_claim_interface(devh, IF_MODEM);
	if (r < 0) {
		debug_msg(0, "claim usb interface error %d", r);
		exit_close_usb(1);
	}
	debug_msg(0, "claimed interface");

	sigact.sa_handler = sighandler_exit;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigact.sa_handler = sighandler_wait_child;
	sigaction(SIGCHLD, &sigact, NULL);

	if (daemonize) {
		debug_msg(0, "Daemonizing...");
		set_debug_level(-1);
		daemon(0, 0);
	}

	alloc_fds();
	libusb_set_pollfd_notifiers(ctx, cb_add_pollfd, cb_remove_pollfd, NULL);

	r = init();
	if (r < 0) {
		debug_msg(0, "init error %d", r);
		exit_release_resources(1);
	}

	tap_fd = tap_open(tap_dev);
	if (tap_fd < 0) {
		debug_msg(0, "failed to allocate tap interface");
		debug_msg(0,
				"You should have TUN/TAP driver compiled in the kernel or as a kernel module.\n"
				"If 'modprobe tun' doesn't help then recompile your kernel.");
		exit_release_resources(1);
	}
	tap_set_hwaddr(tap_fd, tap_dev, wd_status.mac);
	tap_set_mtu(tap_fd, tap_dev, 1386);
	set_coe(tap_fd);
	cb_add_pollfd(tap_fd, POLLIN, NULL);

	debug_msg(0, "Allocated tap interface: %s", tap_dev);

	r = scan_loop();
	if (r < 0) {
		debug_msg(0, "scan_loop error %d", r);
		exit_release_resources(1);
	}

	exit_release_resources(0);
	return 0;
}

