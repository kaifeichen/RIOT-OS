/*
 * Copyright (C) 2016 Sam Kumar <samkumar@berkeley.edu>
 *
 * This is a re-implementation of ethos (originally by Kaspar Schleiser)
 * that creates a reliable multi-channel duplex link over serial.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file LICENSE for more details.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/ipv6.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <netdb.h>

#include <termios.h>

#define MTU 16384

#define TRACE(x)
#define TTY_TIMEOUT_MS (500)

#define case_baudrate(val)    \
    case val:                 \
        *baudrate = B ## val; \
        break

#define BAUDRATE_DEFAULT B115200

#define RESERVED_CHANNEL 0
#define STDIN_CHANNEL 1
#define CMD_CHANNEL 2
#define TUNTAP_CHANNEL 3
#define NUM_CHANNELS 256

/* Commands for REthos (only one for now) */
#define CMD_GET_MCU_IP_ADDR 0x01

/* Return codes for commands for REthos. */
#define RSP_GET_MCU_IP_ADDR 0x11

bool have_stdin = true;

typedef struct {
    uint64_t serial_received;
    uint64_t domain_forwarded;

    uint64_t domain_received;
    uint64_t serial_forwarded;

    uint64_t lost_frames;
    uint64_t bad_frames;

    uint64_t drop_notconnected;
} __attribute__((packed)) global_stats_t;

typedef struct {
    uint64_t serial_received;
    uint64_t domain_forwarded;
    uint64_t drop_notconnected;

    uint64_t domain_received;
    uint64_t serial_forwarded;
} __attribute__((packed)) channel_stats_t;

struct {
    global_stats_t global;
    channel_stats_t channel[NUM_CHANNELS];
} __attribute__((packed)) stats = {0};


/*
 * Code to handle timers.
 * There are three timers: stats, retransmission, "send IP address"
 */
#define STATS_TIMER_TYPE 0
#define REXMIT_TIMER_TYPE 1
#define IPADDR_TIMER_TYPE 2

/* Stats timeout is 15 seconds. Type is struct timespec. */
#define STATS_TIMEOUT {(time_t) 15, 0L}

/* Retransmission timeout is 100 ms. Type is struct timespec. */
#define REXMIT_TIMEOUT {(time_t) 0, 100000000L}

/* IP Address timeout is 20 seconds. Type is struct timespec. */
#define IPADDR_TIMEOUT {(time_t) 20, 0L}

const struct itimerspec stats_timer_spec = { STATS_TIMEOUT, STATS_TIMEOUT };
const struct itimerspec rexmit_timer_spec = { REXMIT_TIMEOUT, REXMIT_TIMEOUT };
const struct itimerspec ipaddr_timer_spec = { IPADDR_TIMEOUT, IPADDR_TIMEOUT };
const struct itimerspec cancel_timer_spec = { {0, 0L}, {0, 0L} };

timer_t stats_timer;
timer_t rexmit_timer;
timer_t ipaddr_timer;

volatile sig_atomic_t stats_fired = 0;
volatile sig_atomic_t rexmit_fired = 0;
volatile sig_atomic_t ipaddr_fired = 0;

/* This function executes in the SIGUSR1 signal handler. */
static void timer_handler(int signum, siginfo_t* info, void* context) {
    (void) signum;
    (void) context;

    int timer_type = info->si_value.sival_int;
    switch (timer_type) {
    case STATS_TIMER_TYPE:
        stats_fired = 1;
        break;
    case REXMIT_TIMER_TYPE:
        rexmit_fired = 1;
        break;
    case IPADDR_TIMER_TYPE:
        ipaddr_fired = 1;
        break;
    }
}

void check_fatal_error(const char* msg)
{
    assert(errno);
    perror(msg);
    exit(1);
}

static void checked_write(int fd, const void* buffer, size_t size)
{
    const char* buf = buffer;
    size_t written = 0;
    while (written < size) {
        ssize_t rv = write(fd, &buf[written], size - written);
        if (rv == -1) {
            char errbuf[50];
            snprintf(errbuf, sizeof(errbuf), "write to fd %d failed", fd);
            check_fatal_error(errbuf);
        }
        written += rv;
    }
}

static void write_message(int handle, const void *buffer, int nbyte)
{
    /* Write 4-byte size in network byte order. */
    uint32_t size = (uint32_t) nbyte;
    size = htonl(size);
    checked_write(handle, &size, sizeof(size));

    /* Write the actual data. */
    checked_write(handle, buffer, (size_t) nbyte);
}

size_t checked_read(int fd, void* buffer, size_t toread)
{
    char* buf = buffer;
    size_t consumed = 0;
    while (consumed < toread) {
        ssize_t rv = read(fd, &buf[consumed], toread - consumed);
        if (rv == -1) {
            char errbuf[50];
            snprintf(errbuf, sizeof(errbuf), "read from fd %d failed", fd);
            check_fatal_error(errbuf);
        } else if (rv == 0) {
            break;
        }
        consumed += rv;
    }
    return consumed;
}

#define READ_SUCCESS 0
#define READ_EOF 1
#define READ_PARTIAL 2
#define READ_OVERFLOW 3
static uint32_t read_message(int* status, int handle, void* buffer, int buffer_size)
{
    uint32_t message_size;
    uint32_t bytes_read = checked_read(handle, &message_size, sizeof(message_size));
    if (bytes_read != sizeof(message_size)) {
        if (bytes_read == 0) {
            *status = READ_EOF;
        } else {
            *status = READ_PARTIAL;
        }
        return 0;
    }
    message_size = ntohl(message_size);

    uint32_t bytes_to_read = message_size;
    if (bytes_to_read > (uint32_t) buffer_size) {
        bytes_to_read = (uint32_t) buffer_size;
    }

    bytes_read = checked_read(handle, buffer, bytes_to_read);
    if (bytes_read != bytes_to_read) {
        *status = READ_PARTIAL;
    }

    if (message_size > (uint32_t) buffer_size) {
        *status = READ_OVERFLOW;
        size_t remaining = message_size - (uint32_t) buffer_size;
        char sbuf;
        while (remaining != 0) {
            checked_read(handle, &sbuf, 1);
            remaining--;
        }
    } else {
        *status = READ_SUCCESS;
    }

    return message_size;
}

int set_serial_attribs (int fd, int speed, int parity)
{
    struct termios tty;
    memset (&tty, 0, sizeof tty);
    if (tcgetattr (fd, &tty) != 0)
    {
        perror ("error in tcgetattr");
        return -1;
    }

    cfsetospeed (&tty, speed);
    cfsetispeed (&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; /* 8-bit chars*/
                                        /* disable IGNBRK for mismatched speed
                                         * tests; otherwise receive break*/
                                        /* as \000 chars*/
    tty.c_iflag &= ~IGNBRK;             /* disable break processing*/
    tty.c_lflag = 0;                    /* no signaling chars, no echo,*/
                                        /* no canonical processing*/
    tty.c_oflag = 0;                    /* no remapping, no delays*/
    tty.c_cc[VMIN]  = 0;                /* read doesn't block*/
    tty.c_cc[VTIME] = TTY_TIMEOUT_MS / 100; /* 0.5 seconds read timeout*/
                                            /* in tenths of a second*/

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); /* shut off xon/xoff ctrl*/

    tty.c_cflag |= (CLOCAL | CREAD);    /* ignore modem controls,*/
                                        /* enable reading*/
    tty.c_cflag &= ~(PARENB | PARODD);  /* shut off parity*/
    tty.c_cflag |= parity;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    cfmakeraw(&tty);

    if (tcsetattr (fd, TCSANOW, &tty) != 0)
    {
        perror("error from tcsetattr");
        return -1;
    }
    return 0;
}

void set_blocking (int fd, int should_block)
{
    struct termios tty;
    memset (&tty, 0, sizeof tty);
    if (tcgetattr (fd, &tty) != 0)
    {
        perror("error from tggetattr");
        return;
    }

    tty.c_cc[VMIN]  = should_block ? 1 : 0;
    tty.c_cc[VTIME] = TTY_TIMEOUT_MS / 100; /* 0.5 seconds read timeout*/
                                            /* in tenths of a second*/

    if (tcsetattr (fd, TCSANOW, &tty) != 0)
        perror("error setting term attributes");
}

/**
 * @name Escape char definitions
 * @{
 */
#define RETHOS_ESC_CHAR                  (0xBE)
/* This means that a stream of ESC_CHAR still keeps us inside the escape state */
#define RETHOS_LITERAL_ESC               (0x55)
#define RETHOS_FRAME_START               (0xEF)
#define RETHOS_FRAME_END                 (0xE5)

#define RETHOS_FRAME_TYPE_DATA           (0x1)

#define RETHOS_FRAME_TYPE_HB             (0x2)
#define RETHOS_FRAME_TYPE_HB_REPLY       (0x3)

#define RETHOS_FRAME_TYPE_ACK            (0x4)
#define RETHOS_FRAME_TYPE_NACK           (0x5)

/** @} */

static const uint8_t _esc_esc[] = {RETHOS_ESC_CHAR, RETHOS_LITERAL_ESC};
static const uint8_t _start_frame[] = {RETHOS_ESC_CHAR, RETHOS_FRAME_START};
static const uint8_t _end_frame[] = {RETHOS_ESC_CHAR, RETHOS_FRAME_END};

typedef enum {
    WAIT_FRAMESTART,
    WAIT_FRAMETYPE,
    WAIT_SEQNO_1,
    WAIT_SEQNO_2,
    WAIT_CHANNEL,
    IN_FRAME,
    WAIT_CHECKSUM_1,
    WAIT_CHECKSUM_2
} line_state_t;

typedef struct {
    int fd;

    /* State for storing partial checksum. */
    uint16_t sum1;
    uint16_t sum2;

    /* State for reading data. */
    line_state_t state;
    uint8_t frametype;
    uint16_t in_seqno;
    uint8_t channel;
    size_t numbytes;
    char frame[MTU];
    uint16_t checksum;

    bool in_escape;

    /* State for writing data. */
    uint16_t out_seqno;

    /* Last data frame sent, used for retransmissions. */
    // rexmit_frametype is always RETHOS_FRAME_TYPE_DATA
    uint16_t rexmit_seqno;
    uint8_t rexmit_channel;
    size_t rexmit_numbytes;
    uint8_t rexmit_frame[MTU];
    bool rexmit_acked;

    /* Keeps track of whether anything has been sent yet. */
    bool received_data_frame;

    /* Last received sequence number, used to detect losses. */
    uint16_t last_rcvd_seqno;
} serial_t;

static void fletcher16_add(const uint8_t *data, size_t bytes, uint16_t *sum1i, uint16_t *sum2i)
{
    uint16_t sum1 = *sum1i, sum2 = *sum2i;

    while (bytes) {
        size_t tlen = bytes > 20 ? 20 : bytes;
        bytes -= tlen;
        do {
            sum2 += sum1 += *data++;
        } while (--tlen);
        sum1 = (sum1 & 0xff) + (sum1 >> 8);
        sum2 = (sum2 & 0xff) + (sum2 >> 8);
    }
    *sum1i = sum1;
    *sum2i = sum2;
}

static uint16_t fletcher16_fin(uint16_t sum1, uint16_t sum2)
{
  sum1 = (sum1 & 0xff) + (sum1 >> 8);
  sum2 = (sum2 & 0xff) + (sum2 >> 8);
  return (sum2 << 8) | sum1;
}

typedef enum {
    NO_EVENT,
    FRAME_READY,
    FRAME_DROPPED
} serial_event_t;

static serial_event_t _serial_handle_byte(serial_t *serial, char c)
{
    serial_event_t event = NO_EVENT;

    if (c == RETHOS_ESC_CHAR) {
        serial->in_escape = true;
        return event;
    }

    if (serial->in_escape) {
        if (c == RETHOS_LITERAL_ESC) {
            c = RETHOS_ESC_CHAR;
            serial->in_escape = false;
        } else if (c == RETHOS_FRAME_START) {
            /* If we receive the start sequence, then drop the current frame and start receiving. */
            if (serial->state != WAIT_FRAMESTART) {
                fprintf(stderr, "Got unexpected start-of-frame sequence: dropping current frame\n");
            }
            serial->sum1 = 0xFF;
            serial->sum2 = 0xFF;
            serial->state = WAIT_FRAMETYPE;
            goto done_char;
        } else if (c == RETHOS_FRAME_END) {
            if (serial->state != IN_FRAME) {
                fprintf(stderr, "Got unexpected end-of-frame sequence: dropping current frame\n");
                goto handle_corrupt_frame;
            }

            serial->state = WAIT_CHECKSUM_1;
            goto done_char;
        } else {
            fprintf(stderr, "Got unexpected escape sequence 0xBE%X: dropping current frame\n", (uint8_t) c);
            goto handle_corrupt_frame;
        }
    }

    switch (serial->state) {
        case WAIT_FRAMESTART:
            fprintf(stderr, "Got stray byte %c\n", c);
            break;
        case WAIT_FRAMETYPE:
            serial->frametype = (uint8_t) c;
            serial->state = WAIT_SEQNO_1;
            break;
        case WAIT_SEQNO_1:
            serial->in_seqno = ((uint8_t) c);
            serial->state = WAIT_SEQNO_2;
            break;
        case WAIT_SEQNO_2:
            serial->in_seqno |= (((uint16_t) c) << 8);
            serial->state = WAIT_CHANNEL;
            break;
        case WAIT_CHANNEL:
            serial->channel = (uint8_t) c;
            serial->state = IN_FRAME;
            serial->numbytes = 0;
            break;
        case IN_FRAME:
            if (serial->numbytes >= MTU) {
                fprintf(stderr, "Dropping runaway frame\n");
                goto handle_corrupt_frame;
            }
            serial->frame[serial->numbytes] = c;
            serial->numbytes++;
            break;
        case WAIT_CHECKSUM_1:
            serial->checksum = (uint8_t) c;
            serial->state = WAIT_CHECKSUM_2;
            goto done_char;
        case WAIT_CHECKSUM_2:
            serial->checksum |= (((uint16_t) c) << 8);

            /* Check the checksum. */
            if (serial->checksum != fletcher16_fin(serial->sum1, serial->sum2)) {
                goto handle_corrupt_frame;
            }

            /* Set "ready" so the frame is delivered to the handler. */
            event = FRAME_READY;

            goto drop_frame_state;
    }

    /* Update checksum. */
    fletcher16_add((uint8_t*) &c, 1, &serial->sum1, &serial->sum2);

    goto done_char;

handle_corrupt_frame:
    /* It's the caller's responsibility to send a NACK if this happens. */
    event = FRAME_DROPPED;

drop_frame_state:
    /* Start listening for a frame at the beginning. */
    serial->state = WAIT_FRAMESTART;

done_char:
    /* Finished handling this character. */
    serial->in_escape = false;
    return event;
}

static void _write_escaped(int fd, const uint8_t* buf, ssize_t n)
{
    for (ssize_t i = 0; i < n; i++) {
        char c = (char) buf[i];
        if (c == RETHOS_ESC_CHAR) {
            checked_write(fd, _esc_esc, sizeof(_esc_esc));
        } else {
            checked_write(fd, &buf[i], 1);
        }
    }
}

void _send_frame(serial_t* serial, const uint8_t *data, size_t thislen, uint8_t channel, uint16_t seqno, uint8_t frame_type)
{
    uint8_t preamble_buffer[4];
    uint8_t postamble_buffer[2];

    uint16_t flsum1 = 0xFF;
    uint16_t flsum2 = 0xFF;

    //This is where the checksum starts
    preamble_buffer[0] = frame_type;
    preamble_buffer[1] = seqno & 0xFF; //Little endian cos im a rebel
    preamble_buffer[2] = seqno >> 8;
    preamble_buffer[3] = channel;

    checked_write(serial->fd, _start_frame, sizeof(_start_frame));

    fletcher16_add(preamble_buffer, sizeof(preamble_buffer), &flsum1, &flsum2);
    _write_escaped(serial->fd, preamble_buffer, sizeof(preamble_buffer));

    fletcher16_add(data, thislen, &flsum1, &flsum2);
    _write_escaped(serial->fd, data, thislen);

    checked_write(serial->fd, _end_frame, sizeof(_end_frame));

    uint16_t cksum = fletcher16_fin(flsum1, flsum2);
    postamble_buffer[0] = (uint8_t) cksum;
    postamble_buffer[1] = (uint8_t) (cksum >> 8);

    _write_escaped(serial->fd, postamble_buffer, sizeof(postamble_buffer));
}

void rethos_send_data_frame(serial_t* serial, const uint8_t* data, size_t datalen, uint8_t channel) {
    uint16_t seqno = ++serial->out_seqno;

    /* Store this data, in case we need to retransmit it. */
    serial->rexmit_seqno = seqno;
    serial->rexmit_channel = channel;
    serial->rexmit_numbytes = datalen;
    memcpy(serial->rexmit_frame, data, datalen);
    serial->rexmit_acked = false;

    _send_frame(serial, data, datalen, channel, seqno, RETHOS_FRAME_TYPE_DATA);

    /* Set the rexmit timer. */
    if (timer_settime(rexmit_timer, 0, &rexmit_timer_spec, NULL) == -1) {
        check_fatal_error("Could not set rexmit timer");
    }
}

void rethos_rexmit_data_frame(serial_t* serial) {
    _send_frame(serial, serial->rexmit_frame, serial->rexmit_numbytes, serial->rexmit_channel, serial->rexmit_seqno, RETHOS_FRAME_TYPE_DATA);
}

void rethos_send_ack_frame(serial_t* serial, uint16_t seqno) {
    _send_frame(serial, NULL, 0, RESERVED_CHANNEL, seqno, RETHOS_FRAME_TYPE_ACK);
}

void rethos_send_nack_frame(serial_t* serial) {
    _send_frame(serial, NULL, 0, RESERVED_CHANNEL, 0, RETHOS_FRAME_TYPE_NACK);
}

static int _parse_baudrate(const char *arg, unsigned *baudrate)
{
    if (arg == NULL) {
        *baudrate = BAUDRATE_DEFAULT;
        return 0;
    }

    switch(strtol(arg, (char**)NULL, 10)) {
    case 9600:
        *baudrate = B9600;
        break;
    case 19200:
        *baudrate = B19200;
        break;
    case 38400:
        *baudrate = B38400;
        break;
    case 57600:
        *baudrate = B57600;
        break;
    case 115200:
        *baudrate = B115200;
        break;
    /* the following baudrates might not be available on all platforms */
    #ifdef B234000
        case_baudrate(230400);
    #endif
    #ifdef B460800
        case_baudrate(460800);
    #endif
    #ifdef B500000
        case_baudrate(500000);
    #endif
    #ifdef B576000
        case_baudrate(576000);
    #endif
    #ifdef B921600
        case_baudrate(921600);
    #endif
    #ifdef B1000000
        case_baudrate(1000000);
    #endif
    #ifdef B1152000
        case_baudrate(1152000);
    #endif
    #ifdef B1500000
        case_baudrate(1500000);
    #endif
    #ifdef B2000000
        case_baudrate(2000000);
    #endif
    #ifdef B2500000
        case_baudrate(2500000);
    #endif
    #ifdef B3000000
        case_baudrate(3000000);
    #endif
    #ifdef B3500000
        case_baudrate(3500000);
    #endif
    #ifdef B4000000
        case_baudrate(4000000);
    #endif
    default:
        return -1;
    }

    return 0;
}

int _open_serial_connection(char *name, char *baudrate_arg)
{
    unsigned baudrate = 0;
    if (_parse_baudrate(baudrate_arg, &baudrate) == -1) {
        fprintf(stderr, "Invalid baudrate specified: %s\n", baudrate_arg);
        return 1;
    }

    int serial_fd = open(name, O_RDWR | O_NOCTTY | O_SYNC);

    if (serial_fd < 0) {
        fprintf(stderr, "Error opening serial device %s\n", name);
        return -1;
    }

    set_serial_attribs(serial_fd, baudrate, 0);
    set_blocking(serial_fd, 1);

    return serial_fd;
}

typedef struct {
    int server_socket;
    int client_socket;
} channel_t;

void channel_listen(channel_t* chan, int channel_number) {
    int dsock;
    int flags;
    struct sockaddr_un bound_name;
    size_t total_size;
    memset(&bound_name, 0, sizeof(bound_name));
    bound_name.sun_family = AF_UNIX;
    snprintf(&bound_name.sun_path[1], sizeof(bound_name.sun_path) - 1, "rethos/%d", channel_number);
    total_size = sizeof(bound_name.sun_family) + 1 + strlen(&bound_name.sun_path[1]);
    dsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (dsock == -1) {
        check_fatal_error("Could not create domain socket");
    }
    flags = fcntl(dsock, F_GETFL);
    if (flags == -1) {
        check_fatal_error("Could not get socket flags");
    }
    flags = fcntl(dsock, F_SETFL, flags | O_NONBLOCK);
    if (flags == -1) {
        check_fatal_error("Could not set socket flags");
    }
    if (bind(dsock, (struct sockaddr*) &bound_name, total_size) == -1) {
        check_fatal_error("Could not bind domain socket");
    }
    if (listen(dsock, 0) == -1) {
        check_fatal_error("Could not listen on domain socket");
    }

    chan->server_socket = dsock;
    chan->client_socket = -1;
}

int main(int argc, char *argv[])
{
    uint8_t inbuf[MTU];

    /* Response frame containing IP address of MCU. */
    struct {
        char opcode;
        struct in6_addr mcu_addr;
    } mcu_addr_rsp_frame;
    mcu_addr_rsp_frame.opcode = RSP_GET_MCU_IP_ADDR;

    /* Parse command line arguments */
    if (argc != 3 && argc != 4) {
        printf("Usage: %s <serial> <baudrate> <ipv6_address>\n", argv[0]);
        printf("The provided ipv6_address is interpreted as a /64 prefix for\n"
               "the subnet. PREFIX::1 is the IP address of this device on\n"
               "the link, and PREFIX::2 is the IP address of the MCU. If\n"
               "no ipv6_address is provided, rethos will not act as a\n"
               "router; it will only forward messages to other processes.\n");
        return 1;
    }

    /* Block the SIGUSR1 signal. */
    sigset_t oldmask;
    sigset_t toblock;
    if (sigemptyset(&toblock) == -1) {
        check_fatal_error("Could not create empty signal set");
    }
    if (sigaddset(&toblock, SIGUSR1) == -1) {
        check_fatal_error("Could not add signal to signal set");
    }
    if (sigprocmask(SIG_BLOCK, &toblock, &oldmask) == -1) {
        check_fatal_error("Could not block signal");
    }

    struct sigaction act;
    memset(&act, 0x00, sizeof(struct sigaction));
    act.sa_sigaction = timer_handler;
    act.sa_flags = SA_SIGINFO;
    if (sigaction(SIGUSR1, &act, NULL) == -1) {
        check_fatal_error("Could not set up signal handler for SIGUSR1");
    }

    serial_t serial = {0};
    serial.rexmit_acked = true; // The retransmit buffer contains garbage, so don't transmit that

    /* Open TUN channel to forward packets. */
    int tun_fd;
    if (argc == 4) {
        tun_fd = open("/dev/net/tun", O_RDWR);
        if (tun_fd == -1) {
            perror("open(\"/dev/net/tun\")");
            return 1;
        }

        struct ifreq ifr;
        memset(&ifr, 0x00, sizeof(ifr));
        ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

        /*
         * We leave ifr.ifr_name full of 0x00, so the kernel will automatically
         * assign a name.
         */
        if (ioctl(tun_fd, TUNSETIFF, &ifr) == -1) {
            perror("ioctl(TUNSETIFF)");
            return 1;
        }
        printf("Created TUN interface: %s\n", ifr.ifr_name);

        int sockfd = socket(AF_INET6, SOCK_DGRAM, 0);
        if (ioctl(sockfd, SIOGIFINDEX, &ifr) == -1) {
            perror("ioctl(SIOGIFINDEX)");
            return 1;
        }

        struct in6_ifreq ifr6;
        ifr6.ifr6_ifindex = ifr.ifr_ifindex;
        ifr6.ifr6_prefixlen = 64;

        /*
         * mcu_addr, for now, is just a placeholder so we can print important
         * IPv6 addresses to keep the user in the loop.
         */
        if (inet_pton(AF_INET6, argv[3], &mcu_addr_rsp_frame.mcu_addr) != 1) {
            printf("Invalid IPv6 address provided: %s\n", argv[3]);
            return 1;
        }

        /* Set last 64 bits of IPv6 address to 0. */
        memset(&mcu_addr_rsp_frame.mcu_addr.s6_addr[8], 0x00, 8);

        char ipstrbuf[64];
        const char* ipstr = inet_ntop(AF_INET6, &mcu_addr_rsp_frame.mcu_addr, ipstrbuf, sizeof(ipstrbuf));
        if (ipstr == NULL) {
            perror("inet_ntop");
            return 1;
        }
        printf("IPv6 subnet is %s/64\n", ipstr);

        mcu_addr_rsp_frame.mcu_addr.s6_addr[15] = 0x01;
        ipstr = inet_ntop(AF_INET6, &mcu_addr_rsp_frame.mcu_addr, ipstrbuf, sizeof(ipstrbuf));
        if (ipstr == NULL) {
            perror("inet_ntop");
            return 1;
        }
        printf("IPv6 address of this device is %s\n", ipstr);

        /* This is the address that we want to set the TUN interface. */
        memcpy(&ifr6.ifr6_addr, &mcu_addr_rsp_frame.mcu_addr, sizeof(struct in6_addr));

        /* Finally, we set mcu_addr to the IPv6 address of the MCU. */
        mcu_addr_rsp_frame.mcu_addr.s6_addr[15] = 0x02;
        ipstr = inet_ntop(AF_INET6, &mcu_addr_rsp_frame.mcu_addr, ipstrbuf, sizeof(ipstrbuf));
        if (ipstr == NULL) {
            perror("inet_ntop");
            return 1;
        }
        printf("IPv6 address of the MCU is %s\n", ipstr);

        /* Actually set the IP address of the tun interface. */
        if (ioctl(sockfd, SIOCSIFADDR, &ifr6) == -1) {
            perror("ioctl(SIOCSIFADDR)");
            return 1;
        }

        /* Bring up the TUN interface. */
        ifr.ifr_flags = IFF_UP | IFF_RUNNING;
        if (ioctl(sockfd, SIOCSIFFLAGS, &ifr) == -1) {
            perror("ioctl(SIOCSIFFLAGS)");
            return 1;
        }

        if (close(sockfd) == -1) {
            perror("close");
        }
    } else {
        tun_fd = -1;
        printf("No IPv6 address provided; will not forward packets\n");
    }

    /* Open serial channel to the MCU. */
    int serial_fd = _open_serial_connection(argv[1], argv[2]);
    if (serial_fd < 0) {
        fprintf(stderr, "Error opening serial device %s\n", argv[1]);
        return 1;
    }

    serial.fd = serial_fd;

    fd_set readfds;

    channel_t domain_sockets[NUM_CHANNELS];
    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        channel_listen(&domain_sockets[i], i);
    }

    struct sigevent sev;
    memset(&sev, 0x00, sizeof(struct sigevent));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    sev.sigev_value.sival_int = STATS_TIMER_TYPE;
    if (timer_create(CLOCK_MONOTONIC, &sev, &stats_timer) == -1) {
        check_fatal_error("Could not create rexmit timer");
    }

    sev.sigev_value.sival_int = REXMIT_TIMER_TYPE;
    if (timer_create(CLOCK_MONOTONIC, &sev, &rexmit_timer) == -1) {
        check_fatal_error("Could not create rexmit timer");
    }

    sev.sigev_value.sival_int = IPADDR_TIMER_TYPE;
    if (timer_create(CLOCK_MONOTONIC, &sev, &ipaddr_timer) == -1) {
        check_fatal_error("Could not create ipaddr timer");
    }

    if (timer_settime(stats_timer, 0, &stats_timer_spec, NULL) == -1) {
        check_fatal_error("Could not set stats timer");
    }

    if (timer_settime(ipaddr_timer, 0, &ipaddr_timer_spec, NULL) == -1) {
        check_fatal_error("Could not set ipaddr timer");
    }

    while (true) {
        int activity;
        int max_fd = 0;
        #define UPDATE_MAX_FD(fd) max_fd = ((fd) > max_fd ? (fd) : max_fd);
        FD_ZERO(&readfds);
        if (have_stdin) {
            FD_SET(STDIN_FILENO, &readfds);
            UPDATE_MAX_FD(STDIN_FILENO);
        }
        if (tun_fd != -1) {
            FD_SET(tun_fd, &readfds);
            UPDATE_MAX_FD(tun_fd);
        }
        FD_SET(serial_fd, &readfds);
        UPDATE_MAX_FD(serial_fd);
        for (i = 0; i < NUM_CHANNELS; i++) {
            if (domain_sockets[i].client_socket == -1) {
                FD_SET(domain_sockets[i].server_socket, &readfds);
                UPDATE_MAX_FD(domain_sockets[i].server_socket);
            } else {
                FD_SET(domain_sockets[i].client_socket, &readfds);
                UPDATE_MAX_FD(domain_sockets[i].client_socket);
            }
        }

        activity = pselect(max_fd + 1, &readfds, NULL, NULL, NULL, &oldmask);

        if (activity == -1 && (errno != EINTR || (stats_fired == 0 && rexmit_fired == 0 && ipaddr_fired == 0))) {
            check_fatal_error("Could not wait for event");
        }

        if (stats_fired == 1) {
            stats_fired = 0;

            printf("================================================================================\n");
            printf("Received %" PRIu64 " frames on serial link; forwarded %" PRIu64 " on domain sockets\n", stats.global.serial_received, stats.global.domain_forwarded);
            printf("Received %" PRIu64 " frames on domain sockets; forwarded %" PRIu64 " on serial link\n", stats.global.domain_received, stats.global.serial_forwarded);
            printf("Lost %" PRIu64 " frames, %" PRIu64 " of which were detected on the serial link\n", stats.global.lost_frames, stats.global.bad_frames);
            printf("An additional %" PRIu64 " frames were dropped, due to lack of a listening process\n", stats.global.drop_notconnected);

            if (domain_sockets[RESERVED_CHANNEL].client_socket != -1) {
                write_message(domain_sockets[RESERVED_CHANNEL].client_socket, &stats, sizeof(stats));
            }
        }

        if (rexmit_fired == 1) {
            rexmit_fired = 0;

            if (!serial.rexmit_acked) {
                rethos_rexmit_data_frame(&serial);
            }
        }

        if (ipaddr_fired == 1) {
            ipaddr_fired = 0;

            rethos_send_data_frame(&serial, (uint8_t*) &mcu_addr_rsp_frame, sizeof(mcu_addr_rsp_frame), CMD_CHANNEL);
        }

        if (activity == -1) {
            /*
             * The file descriptor sets were unmodified. The only reason pselect
             * returned was because of the signal. So readfds just contains
             * what we originally put there, and there are really no file
             * descriptors that are ready.
             */
            continue;
        }

        if (FD_ISSET(serial_fd, &readfds)) {
            ssize_t n = read(serial_fd, (char*) inbuf, sizeof(inbuf));
            if (n > 0) {
                char* ptr = (char*) inbuf;
                for (i = 0; i != n; i++) {
                    serial_event_t event = _serial_handle_byte(&serial, *ptr++);
                    if (event == FRAME_READY) {
                        if (serial.channel >= NUM_CHANNELS) {
                            continue;
                        }

                        stats.channel[serial.channel].serial_received++;
                        stats.global.serial_received++;

                        /* Use the sequence number and
                         * message type for reliable delivery. */
                        if (serial.channel == RESERVED_CHANNEL) {
                            if (serial.frametype == RETHOS_FRAME_TYPE_NACK) {
                                /* If we got a NACK for something that was already ACKed, it's
                                 * probably because we sent a NACK that got corrupted, or an ACK
                                 * that got corrupted.
                                 *
                                 * Sending a NACK could cause a NACK storm, so instead just ACK
                                 * the last packet we received.
                                 */
                                if (serial.rexmit_acked) {
                                    if (serial.received_data_frame)
                                    {
                                        rethos_send_ack_frame(&serial, serial.last_rcvd_seqno);
                                    }
                                } else {
                                    /* Retransmit the last frame that was sent. */
                                    rethos_rexmit_data_frame(&serial);
                                }
                            } else if (serial.frametype == RETHOS_FRAME_TYPE_ACK) {
                                if (serial.in_seqno == serial.rexmit_seqno) {
                                    /* Mark the frame to be retransmitted as having been ACKed so we don't retransmit it on timeout. */
                                    serial.rexmit_acked = true;

                                    /* Cancel the pending rexmit timer. */
                                    if (timer_settime(rexmit_timer, 0, &cancel_timer_spec, NULL) == -1) {
                                        check_fatal_error("Could not cancel rexmit timer");
                                    }
                                }
                            } else {
                                printf("Got frame of type %d on control channel\n", serial.frametype);
                            }
                            goto serial_done;
                        }

                        /* ACK the frame we just received. */
                        rethos_send_ack_frame(&serial, serial.in_seqno);

                        /* If it's a duplicate, just drop the frame. */
                        if (serial.received_data_frame && (serial.in_seqno == serial.last_rcvd_seqno)) {
                            printf("Got a duplicate frame on channel %d\n", serial.channel);
                            goto serial_done;
                        }

                        serial.received_data_frame = true;

                        /* Record the number of lost frames. */
                        stats.global.lost_frames += (serial.in_seqno - serial.last_rcvd_seqno - 1);
                        serial.last_rcvd_seqno = serial.in_seqno;

                        if (serial.numbytes == 0) {
                            printf("Got an empty frame on channel %d: dropping frame\n", serial.channel);
                            goto serial_done;
                        } else {
                            printf("Got a frame on channel %d\n", serial.channel);
                        }

                        if (serial.channel == STDIN_CHANNEL) {
                            checked_write(STDOUT_FILENO, serial.frame, serial.numbytes);
                        } else if (serial.channel == TUNTAP_CHANNEL) {
                            if (tun_fd == -1) {
                                printf("Got a packet to forward: dropping it\n");
                            } else {
                                ssize_t written = write(tun_fd, serial.frame, serial.numbytes);
                                if (written == -1) {
                                    perror("write(tun_fd)");
                                }
                                if (written != serial.numbytes) {
                                    fprintf(stderr, "Sent partial packet: packet size is %zu bytes, but write(tun_fd) returned %zd\n", serial.numbytes, written);
                                }
                            }
                        } else if (serial.channel == CMD_CHANNEL) {
                            /* This is a request to REthos itself. */
                            if (serial.numbytes == 0) {
                                printf("Got empty command\n");
                            } else {
                                uint8_t opcode = serial.frame[0];
                                switch (opcode) {
                                case CMD_GET_MCU_IP_ADDR:
                                    printf("Got command: Get MCU IP Address\n");
                                    rethos_send_data_frame(&serial, (uint8_t*) &mcu_addr_rsp_frame, sizeof(mcu_addr_rsp_frame), CMD_CHANNEL);
                                    break;
                                }
                            }
                        }

                        if (domain_sockets[serial.channel].client_socket != -1) {
                            write_message(domain_sockets[serial.channel].client_socket, serial.frame, serial.numbytes);
                            stats.channel[serial.channel].domain_forwarded++;
                            stats.global.domain_forwarded++;
                        } else {
                            fprintf(stderr, "Got message on channel %d, which is not connected: dropping message\n", serial.channel);
                            stats.channel[serial.channel].drop_notconnected++;
                            if (serial.channel != STDIN_CHANNEL && serial.channel != TUNTAP_CHANNEL) {
                                stats.global.drop_notconnected++;
                            }
                        }
                    } else if (event == FRAME_DROPPED) {
                        stats.global.bad_frames++;
                        stats.global.lost_frames++;

                        /* Send a NACK. */
                        rethos_send_nack_frame(&serial);
                    }
                }
            }
            else {
                fprintf(stderr, "lost serial connection.\n");
                exit(1);
            }
        }

    serial_done:
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t res = read(STDIN_FILENO, inbuf, sizeof(inbuf));
            if (res <= 0) {
                fprintf(stderr, "Error reading from stdio. res=%zi. Disabling stdin functionality...\n", res);
                have_stdin = false;
                continue;
            }
            rethos_send_data_frame(&serial, inbuf, res, STDIN_CHANNEL);
        }

        if (tun_fd != -1 && FD_ISSET(tun_fd, &readfds)) {
            ssize_t res = read(tun_fd, inbuf, sizeof(inbuf));
            if (res <= 0) {
                perror("read(tun_fd)");
                continue;
            }
            rethos_send_data_frame(&serial, inbuf, res, TUNTAP_CHANNEL);
        }

        for (i = 0; i < NUM_CHANNELS; i++) {
            int dsock;
            if (domain_sockets[i].client_socket == -1) {
                dsock = domain_sockets[i].server_socket;
                if (FD_ISSET(dsock, &readfds)) {
                    struct sockaddr_un client_addr;
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_socket = accept(dsock, (struct sockaddr*) &client_addr, &client_addr_len);
                    if (client_socket == -1) {
                        check_fatal_error("accept connection on domain socket");
                    }
                    printf("Accepted client process on channel %d\n", i);
                    domain_sockets[i].client_socket = client_socket;

                    /* Stop listening on this channel. It only makes sense to have one entity listening and writing. */
                    close(domain_sockets[i].server_socket);
                    domain_sockets[i].server_socket = -1;
                }
            } else {
                dsock = domain_sockets[i].client_socket;
                if (FD_ISSET(dsock, &readfds)) {
                    int status;
                    uint32_t message_size = read_message(&status, dsock, inbuf, sizeof(inbuf));

                    switch (status) {
                    case READ_SUCCESS:
                        stats.channel[i].domain_received++;
                        stats.global.domain_received++;
                        rethos_send_data_frame(&serial, inbuf, message_size, i);
                        stats.channel[i].serial_forwarded++;
                        stats.global.serial_forwarded++;
                        break;
                    case READ_OVERFLOW:
                        fprintf(stderr, "frame too big; skipping\n");
                        break;
                    case READ_PARTIAL:
                        fprintf(stderr, "read from domain socket (fd %d) failed: closing\n", domain_sockets[i].client_socket);
                        /* fallthrough intentional */
                    case READ_EOF:
                        close(dsock);
                        domain_sockets[i].client_socket = -1;
                        channel_listen(&domain_sockets[i], i);
                        printf("Client process on channel %d disconnected\n", i);
                        break;
                    }
                }
            }
        }
    }

    return 0;
}