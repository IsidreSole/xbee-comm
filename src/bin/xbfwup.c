/*
 * xbfwup: program new firmware to your xbee
 * Copyright (C) 2011  Joshua Roys
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _BSD_SOURCE

#include <endian.h>
#include <err.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

void
wait_for_ok(int fd) {
	char buf[1];

	do {
		if (read(fd, buf, 1) > 0 && *buf == 'O')
			break;
		usleep(100000);
	} while (1);
	read(fd, buf, 1);
	read(fd, buf, 1);
}

ssize_t
xb_read(int fd, char *buf, size_t count) {
	ssize_t pos = 0, ret;
	struct pollfd fds[1];

	fds[0].fd = fd;
	fds[0].events = POLLIN;

	do {
		ret = read(fd, buf + pos, count - pos);

		if (ret <= 0) {
			return ret;
		}

		pos += ret;

		ret = poll(fds, 1, 100);
	} while (ret > 0 && pos < count);

	return pos;
}

ssize_t
xb_write(int fd, const char *buf, size_t count) {
	ssize_t ret;

	while (count > 0) {
		ret = write(fd, buf, count);

		if (ret <= 0) {
			return -1;
		}

		count -= ret;
		buf += ret;
	}

	return 0;
}

int
xb_send_command(int fd, char *cmd, const char *format, ...) {
	char buf[256];
	size_t off;
	ssize_t ret;
	va_list ap;

//	if (api_mode)
//	else
	buf[0] = 'A';
	buf[1] = 'T';
	buf[2] = cmd[0];
	buf[3] = cmd[1];
	off = 4;

	if (*format) {
		va_start(ap, format);
		ret = vsnprintf(buf + off, sizeof(buf) - off, format, ap);
		va_end(ap);

		if (ret < 0 || ret >= sizeof(buf)) {
			return -1;
		}

		off += ret;
	}

//	if (api_mode)
//	else
	buf[off++] = '\r';

	ret = xb_write(fd, buf, off);

//	if (api_mode)
//	else
	wait_for_ok(fd);

	return ret;
}

/* http://en.wikipedia.org/wiki/Computation_of_CRC */
uint16_t
xmodem_crc(const char *data) {
	int i, j;
	uint16_t rem = 0;

	/* assume 128 byte blocks */
	for(i = 0; i < 128; i++) {
		rem ^= data[i] << 8;
		for(j = 0; j < 8; j++) {
			if (rem & 0x8000) {
				rem = (rem << 1) ^ 0x1021;
			}
			else {
				rem <<= 1;
			}
		}
	}

//	printf("%0hx\n", rem);

	return rem;
}

int
xb_firmware_update(int xbfd, int fwfd) {
	char *fwbuf;
	char buf[128], header[3], reply[1];
	size_t len, off;
	ssize_t ret;
	struct stat stbuf;
	uint8_t block;
	uint16_t crc;
	unsigned int i;

	/* at the menu: "1. upload ebl" */
	if (xb_write(xbfd, "1", 1)) {
		warnx("failed to enter programming mode");
		return -1;
	}

	/* read reply: "\r\nbegin upload\r\nC" */
	if ( (ret = xb_read(xbfd, buf, sizeof(buf))) <= 0) {
		warnx("failed to read programming go-ahead");
		return -1;
	}
	if (buf[ret - 1] != 'C') {
		warnx("unknown transfer type");
		return -1;
	}

	/* read entire firmware image into memory */
	if (fstat(fwfd, &stbuf) < 0) {
		warn("failed to stat firmware file");
		return -1;
	}
	if (stbuf.st_size == 0) {
		warnx("empty firmware file!");
		return -1;
	}

	/* round up to nearest 128 byte block */
	len = stbuf.st_size;
	len += (len % 128 ? 128 - (len % 128) : 0);

	if ( (fwbuf = malloc(len)) == NULL) {
		warn("failed to allocate memory");
		return -1;
	}

	/* set "empty" bytes to 0xff */
	memset(fwbuf + stbuf.st_size, 0xff, len - stbuf.st_size);

	off = 0;
	do {
		ret = read(fwfd, fwbuf + off, len - off);

		if (ret < 0) {
			return -1;
		}

		off += ret;
	} while (ret);

	printf("Read %i byte firmware file (%i blocks).\n",
			(int)stbuf.st_size, (int)len / 128);

	/* send it */
	for(block = 1, i = 0; i < len / 128; block++, i++) {
		header[0] = (uint8_t)'\x01'; /* SOH */
		header[1] = (uint8_t)block;
		header[2] = (uint8_t)(255 - block);

		if (xb_write(xbfd, header, 3)) {
			warn("failed to write XMODEM header, block %i", i);
			return -1;
		}

		if (xb_write(xbfd, fwbuf + (i * 128), 128)) {
			warn("failed to write XMODEM data, block %i", i);
			return -1;
		}

		/* CRC-16 */
		crc = xmodem_crc(fwbuf + (i * 128));
		crc = htobe16(crc);
		if (xb_write(xbfd, (const char *)&crc, 2)) {
			warn("failed to write XMODEM CRC, block %i", i);
			return -1;
		}

		/* read ACK (0x06); use read for speed! */
		if (read(xbfd, reply, 1) <= 0 || *reply != '\x06') {
			warnx("failed to transfer block %i: %02x", i, *reply);
			return -1;
		}

		/* display progress */
		printf(".");
		if ((i + 1) % 50 == 0) {
			printf(" %4i\n", (i + 1));
		}
		fflush(stdout);
	}

	printf("\n");

	/* write EOT (0x04) */
	if (xb_write(xbfd, "\x04", 1)) {
		warn("failed to write XMODEM EOT");
		return -1;
	}

	/* read reply: "\x06\r\nSerial upload complete\r\n" */
	if (xb_read(xbfd, buf, sizeof(buf)) <= 0 || *buf != '\x06') {
		warnx("failed to read programming confirmation");
		return -1;
	}

	return 0;
}

int
main(int argc, char *argv[]) {
	char buf[1024];
	int fwfd, xbfd, i;
	ssize_t ret;
	struct termios serial;

	if (argc < 2) {
		errx(EXIT_FAILURE, "missing parameter: %s <file.ebl>", argv[0]);
	}

	if ( (fwfd = open(argv[1], O_RDONLY)) < 0) {
		err(EXIT_FAILURE, "failed to open firmware file: %s", argv[1]);
	}

	if ( (xbfd = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY)) < 0) {
		err(EXIT_FAILURE, "failed to open serial console");
	}

	if (tcgetattr(xbfd, &serial)) {
		err(EXIT_FAILURE, "failed to get terminal attributes");
	}

//	if (!recovery_mode)

	/* enter command mode */
	printf("Entering AT command mode...\n");
	sleep(1);
	write(xbfd, "+++", 3);
	sleep(1);
	wait_for_ok(xbfd);

	printf("Entering bootloader...\n");

	/* start the power cycle */
	xb_send_command(xbfd, "FR", "");

	/* assert DTR, clear RTS */
	i = TIOCM_DTR | TIOCM_CTS;
	ioctl(xbfd, TIOCMSET, &i);

	/* send a serial break */
	ioctl(xbfd, TIOCSBRK);

	/* wait for the power cycle to hit */
	sleep(2);

	/* clear the serial break */
	ioctl(xbfd, TIOCCBRK);

	/* RTS/CTS have an annoying habit of toggling... */
	i = TIOCM_DTR | TIOCM_CTS;
	ioctl(xbfd, TIOCMSET, &i);

	/* send a carriage return at 115200bps */
	cfsetspeed(&serial, B115200);
	/* don't wait more than 1/10s for input */
	serial.c_cc[VMIN] = 0;
	serial.c_cc[VTIME] = 1;
	if (tcsetattr(xbfd, TCSANOW, &serial)) {
		err(EXIT_FAILURE, "failed to set 115200bps, VMIN/VTIME");
	}

	for(i = 0; i < 20; i++) {
		xb_write(xbfd, "\r", 1);
		if ( (ret = xb_read(xbfd, buf, sizeof(buf))) > 0) {
			break;
		}
	}

	/* check for "BL >" prompt */

	/* restore "wait forever" settings */
	serial.c_cc[VMIN] = 1;
	serial.c_cc[VTIME] = 0;
	if (tcsetattr(xbfd, TCSANOW, &serial)) {
		err(EXIT_FAILURE, "failed to reset VMIN/VTIME");
	}

	printf("Beginning programming...\n");

	/* update! */
	if (xb_firmware_update(xbfd, fwfd)) {
		errx(EXIT_FAILURE, "failed to flash firmware!");
	}

	/* verify */

	printf("Programming complete, running uploaded firmware...\n");

	/* run the firmware */
	xb_write(xbfd, "2", 1);

	/* cleanup */
	cfsetspeed(&serial, B9600);
	if (tcsetattr(xbfd, TCSANOW, &serial)) {
		err(EXIT_FAILURE, "failed to set 9600bps");
	}

	close(fwfd);
	close(xbfd);

	return EXIT_SUCCESS;
}