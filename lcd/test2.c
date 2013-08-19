/*
 * Copyright 2013 Ian Pilcher <arequipeno@gmail.com>
 *
 * This program is free software.  You can redistribute it or modify it under
 * the terms of version 2 of the GNU General Public License (GPL), as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY -- without even the implied warranty of MERCHANTIBILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the text of the GPL for more details.
 *
 * Version 2 of the GNU General Public License is available at:
 *
 *   http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

static int pic_gpio_is_exported(void)
{
	struct stat st;
	int ret;

	ret = stat("/sys/class/gpio/gpio15", &st);
	if (ret == -1) {
		if (errno == ENOENT) {
			return 0;
		}
		else {
			perror("stat: /sys/class/gpio/gpio15");
			exit(__LINE__);
		}
	}

	return 1;
}

static void export_pic_gpio(void)
{
	int fd;

	fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd == -1) {
		perror("open: /sys/class/gpio/export");
		exit(__LINE__);
	}

	if (write(fd, "15", 2) != 2) {
		perror("write: /sys/class/gpio/export");
		exit(__LINE__);
	}

	if (close(fd) == -1) {
		perror("close: /sys/class/gpio/export");
		exit(__LINE__);
	}
}

static void set_pic_gpio_direction(void)
{
	int fd;

	fd = open("/sys/class/gpio/gpio15/direction", O_WRONLY);
	if (fd == -1) {
		perror("open: /sys/class/gpio/gpio15/direction");
		exit(__LINE__);
	}

	if (write(fd, "out", 3) != 3) {
		perror("write: /sys/class/gpio/gpio15/direction");
		exit(__LINE__);
	}

	if (close(fd) == -1) {
		perror("close: /sys/class/gpio/gpio15/direction");
		exit(__LINE__);
	}
}

static void setup_pic_gpio(void)
{
	if (!pic_gpio_is_exported())
		export_pic_gpio();
	set_pic_gpio_direction();
}

static void reset_pic(void)
{
	struct timespec delay;
	int fd;

	fd = open("/sys/class/gpio/gpio15/value", O_WRONLY);
	if (fd == -1) {
		perror("open: /sys/class/gpio/gpio15/value");
		exit(__LINE__);
	}

	if (write(fd, "1", 1) != 1) {
		perror("write: /sys/class/gpio/gpio15/value");
		exit(__LINE__);
	}

	delay.tv_sec = 0;
	delay.tv_nsec = 60000; /* 60 usec */

	if (nanosleep(&delay, NULL) == -1) {
		perror("nanosleep");
		exit(__LINE__);
	}

	if (write(fd, "0", 1) != 1) {
		perror("write: /sys/class/gpio/gpio15/value");
		exit(__LINE__);
	}

	if (close(fd) == -1) {
		perror("close: /sys/class/gpio/gpio15/value");
		exit(__LINE__);
	}

	/* Must wait 2 seconds after resetting PIC */
	delay.tv_sec = 2;
	delay.tv_nsec = 0;

	if (nanosleep(&delay, NULL) == -1) {
		perror("nanosleep");
		exit(__LINE__);
	}
}

static int open_tty(void)
{
	struct termios tio;
	int fd;

	fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY);
	if (fd == -1) {
		perror("open: /dev/ttyS0");
		exit(__LINE__);
	}

	if (tcgetattr(fd, &tio) == -1) {
		perror("tcgetattr: /dev/ttyS0");
		exit(__LINE__);
	}

	/*
	 * Thecus software calls tcflush twice for some reason; maybe they
	 * hit this bug?
	 *
         * http://lkml.indiana.edu/hypermail/linux/kernel/0707.3/1776.html
	 */
	if (tcflush(fd, TCIFLUSH) == -1) {
		perror("tcflush: /dev/ttyS0");
		exit(__LINE__);
	}

        tio.c_iflag = IGNPAR;
        tio.c_oflag = 0;
        tio.c_cflag = CLOCAL | HUPCL | CREAD | CS8 | B9600;
        tio.c_lflag = 0;
        tio.c_cc[VTIME] = 0;
        tio.c_cc[VMIN] = 1;

	if (cfsetospeed(&tio, B9600) == -1) {
		perror("cfsetospeed");
		exit(__LINE__);
	}

	if (tcsetattr(fd, TCSANOW, &tio) == -1) {
		perror("tcsetattr: /dev/ttyS0");
		exit(__LINE__);
	}

	/* Check that tcsetattr actually made *all* changes */
        if (tcgetattr(fd, &tio) == -1) {
                perror("tcgetattr: /dev/ttyS0");
                exit(__LINE__);
        }

	if (		tio.c_iflag != IGNPAR ||
			tio.c_oflag != 0 ||
			tio.c_cflag != (CLOCAL | HUPCL | CREAD | CS8 | B9600) ||
			tio.c_lflag != 0 ||
			tio.c_cc[VTIME] != 0 ||
			tio.c_cc[VMIN] != 1 ||
			cfgetospeed(&tio) != B9600	) {
		fputs("tcsetattr: /dev/ttyS0: Incomplete\n", stderr);
		exit(__LINE__);
	}

	return fd;
}

static void write_msg(int tty_fd, const void *msg_body, size_t msg_body_size)
{
	static unsigned char buf[1024];
	static unsigned char seq = 0;
	ssize_t ret;
	size_t i;

	if (msg_body_size > 255) {
		fputs("Message too large\n", stderr);
		return;
	}

	buf[0] = 0x02;
	buf[1] = seq++;
	buf[2] = 0x00;
	buf[3] = (unsigned char)msg_body_size;
	memcpy(buf + 4, msg_body, msg_body_size);
	buf[msg_body_size + 4] = 0x03;

	printf("Sending %zu bytes:", msg_body_size + 5);
        for (i = 0; i < msg_body_size + 5; ++i) {
                printf(" %02x", buf[i]);
        }
        putchar('\n');

	ret = write(tty_fd, buf, msg_body_size + 5);
	if (ret != (ssize_t)msg_body_size + 5) {
		perror("write: /dev/ttyS0");
		exit(__LINE__);
	}
}

static inline void get_line(char *buf, size_t size)
{
	char *nl;

	fgets(buf, size, stdin);
	nl = strchr(buf, '\n');
	if (nl != NULL)
		*nl = 0;
}

static const unsigned char btmsg_hdr[32] = {
	0x1d, 0x61, 0x67, 0x65, 0x6e, 0x74, 0x32, 0x00, 0xb4, 0x0c, 0x00
};

static void do_btmsg(int tty_fd, const char *msg)
{
	unsigned char msg_body[32];
	size_t msg_len;

	msg_len = strlen(msg);
	if (msg_len > 20)
		msg_len = 20;

	memcpy(msg_body, btmsg_hdr, sizeof btmsg_hdr);
	memcpy(msg_body + 11, msg, msg_len);

	write_msg(tty_fd, msg_body, sizeof msg_body);
}

static void do_setbto_100(int tty_fd, int bto)
{
	unsigned char setbto_100_msg[2] = { 0x13 };

	if (bto < 0 || bto > 255) {
		fputs("Invalid BTO value\n", stderr);
		return;
	}

	setbto_100_msg[1] = (unsigned char)bto;
	write_msg(tty_fd, setbto_100_msg, sizeof setbto_100_msg);
}

static void do_setlogo(int tty_fd, const char *logo)
{
	char msg_buf[255];
	size_t logo_len = strlen(logo);

	if (logo_len > sizeof msg_buf - 1) {
		fputs("Logo too long\n", stderr);
		return;
	}

	msg_buf[0] = 0x11;
	memcpy(msg_buf + 1, logo, logo_len);
	write_msg(tty_fd, msg_buf, logo_len + 1);
}

static const unsigned char MAGIC0[] = { 0x15 };

static const unsigned char MAGIC1[] = { 0x1e };

/* Thecus software sends this one with sequence number 1 */
static const unsigned char MAGIC2[] = { 0x31, 0x00, 0xff };

static const unsigned char MAGIC3[] = {
	0x1c, 0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00,
	0x49, 0x15, 0xcd, 0x5b, 0x3d, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const unsigned char MAGIC4[] = { 0x19 };

static void do_startwd_setexcfg_90(int tty_fd)
{
	write_msg(tty_fd, MAGIC0, sizeof MAGIC0);
	write_msg(tty_fd, MAGIC1, sizeof MAGIC1);
	write_msg(tty_fd, MAGIC2, sizeof MAGIC2);
	write_msg(tty_fd, MAGIC3, sizeof MAGIC3);
	write_msg(tty_fd, MAGIC4, sizeof MAGIC4);
}


static void do_status_msg(int tty_fd, const char *msg)
{
	unsigned char buf[34];
	size_t msg_len;

	msg_len = strlen(msg);
	if (msg_len > 33) {
		fputs("Message too long\n", stderr);
		return;
	}

	memset(buf, 0, sizeof buf);
	buf[0] = 0x19;
	memcpy(buf +1, msg, msg_len);

	write_msg(tty_fd, buf, sizeof buf);
}

static FILE *open_output(int argc, char *argv[])
{
	FILE *out;

	if (argc < 2) {
		fputs("No output device specified\n", stderr);
		return NULL;
	}

	out = fopen(argv[1], "w");
	if (out == NULL) {
		perror("fopen");
		exit(__LINE__);
	}

	fputs("\nReading from /dev/ttyS0:\n\n", out);

	return out;
}

struct output_thread_params {
	FILE	*out_tty;
	int	ttyS0_fd;
};

static void *output_thread_fn(void *arg)
{
	struct output_thread_params *params = arg;
	static unsigned char buf[1024];
	ssize_t i, bytes_read;

	while (1) {

		bytes_read = read(params->ttyS0_fd, buf, sizeof buf);
		if (bytes_read == -1) {
			perror("read: /dev/ttyS0");
			exit(__LINE__);
		}

		if (params->out_tty == NULL)
			continue;

		fprintf(params->out_tty, "Read %zd bytes:", bytes_read);
		for (i = 0; i < bytes_read; ++i)
			fprintf(params->out_tty, " %02hx", buf[i]);
		fputc('\n', params->out_tty);
	}
}

static void start_output_thread(int argc, char *argv[], int ttyS0_fd)
{
	struct output_thread_params params;
	pthread_t tid;

	params.out_tty = open_output(argc, argv);
	params.ttyS0_fd = ttyS0_fd;

	errno = pthread_create(&tid, NULL, output_thread_fn, &params);
	if (errno != 0) {
		perror("pthread_create");
		exit(__LINE__);
	}
}

static char **prompt_for_menu(void)
{
	static char buf[32];
	char **items;
	int i, num_items;
	size_t item_len;

	fputs("Number of menu items: ", stdout);
	fgets(buf, sizeof buf, stdin);
	num_items = atoi(buf);

	if (num_items < 1 || num_items > 10) {
		fputs("Invalid item count\n", stderr);
		return NULL;
	}

	items = malloc((num_items + 1) * sizeof *items);
	items[num_items] = NULL;

	for (i = 0; i < num_items; ++i) {
		printf("Enter item %d: ", i);
		get_line(buf, sizeof buf);
		item_len = strlen(buf);
		items[i] = malloc(item_len + 1);
		memcpy(items[i], buf, item_len + 1);
	}

	return items;
}

static char **prompt_for_message(void)
{
	static char buf[32];
	char **lines;
	size_t line_size;
	int i;

	lines = malloc(2 * sizeof *lines);

	for (i = 0; i < 2; ++i) {
		printf("Enter line %d: ", i + 1);
		get_line(buf, sizeof buf);
		line_size = strlen(buf) + 1;
		lines[i] = malloc(line_size);
		memcpy(lines[i], buf, line_size);
	}

	return lines;
}

static void do_menu(int tty_fd, char **items)
{
	static unsigned char buf[213]; /* up to 10 messages */
	int i, num_items;
	size_t item_len;

	for (num_items = 0; items[num_items] != NULL; ++num_items);

	memset(buf, 0, sizeof buf);
	buf[0] = 0x16;
	buf[1] = 0x00;
	buf[2] = (unsigned char)num_items;

	for (i = 0; i < num_items; ++i) {
		item_len = strlen(items[i]);
		if (item_len > 20)
			item_len = 20;
		memcpy(buf + 3 + i * 21, items[i], item_len);
		free(items[i]);
	}

	free(items);

	write_msg(tty_fd, buf, num_items * 21 + 3);
}

static void do_message(int tty_fd, char **lines)
{
	static unsigned char buf[61];
	size_t line_len;

	memset(buf, ' ', sizeof buf);
	buf[0] = 0x11;

	line_len = strlen(lines[0]);
	if (line_len > 20)
		line_len = 20;
	memcpy(buf + 1, lines[0], line_len);

	line_len = strlen(lines[1]);
	if (line_len > 20)
		line_len = 20;
	memcpy(buf + 41, lines[1], line_len);

	free(lines[0]);
	free(lines[1]);
	free(lines);

	write_msg(tty_fd, buf, sizeof buf);
}

static const char prompt[] =
	"Select command:\n"
	"\n"
	" 1) RESET_PIC\n"
	" 2) SETBTO 100\n"
	" 3) SETLOGO\n"
	" 4) BTMSG\n"
	" 5) STARTWD & SETEXCFG 90\n"
	" 6) LCM_MSG (UPGRADE)\n"
	" 7) Status Message\n"
	" 8) Menu\n"
	" 9) 2-Line Message\n"
	" 0) Quit\n"
	"\n"
	"==> ";

int main(int argc, char *argv[])
{
	static char input[255];
	int tty_fd, selection;
	char **items;

	puts("Configuring PIC GPIO");
	setup_pic_gpio();
	//puts("Resetting PIC");
	//reset_pic();
	tty_fd = open_tty();
	//start_output_thread(argc, argv, tty_fd);
	//puts("Setting BTO to 100");
	//do_setbto_100(tty_fd, 100);

	while (1) {
		fputs(prompt, stdout);
		fgets(input, sizeof input, stdin);
		selection = atoi(input);

		switch (selection) {

			case 0:		goto break_loop;

			case 1:		puts("Resetting PIC");
					reset_pic();
					break;

			case 2: 	fputs("Enter BTO value: ", stdout);
					get_line(input, sizeof input);
					do_setbto_100(tty_fd, atoi(input));
					break;

			case 3: 	fputs("Enter logo: ", stdout);
					get_line(input, sizeof input);
					do_setlogo(tty_fd, input);
					break;

			case 4: 	fputs("Enter message: ", stdout);
					get_line(input, sizeof input);
					do_btmsg(tty_fd, input);
					break;

			case 5:		puts("Sending STARTWD and SETEXCFG 90");
					do_startwd_setexcfg_90(tty_fd);
					break;

			case 7:		fputs("Enter message: ", stdout);
					get_line(input, sizeof input);
					do_status_msg(tty_fd, input);
					break;

			case 8:		items = prompt_for_menu();
					do_menu(tty_fd, items);
					break;

			case 9:		items = prompt_for_message();
					do_message(tty_fd, items);
					break;

			default:	puts("Invalid/unimplemented selection\n");
					break;
		}
	}

break_loop:

	return 0;
}
