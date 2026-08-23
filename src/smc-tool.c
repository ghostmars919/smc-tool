/*
 * smc-tool - read/write Apple SMC keys from userspace
 *
 * SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Simon Gelso
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/io.h>

#define DATA_PORT 0x300
#define CMD_PORT  0x304
#define NR_PORTS  32

#define ST_AWAITING_DATA 0x01
#define ST_IB_CLOSED     0x02
#define ST_BUSY          0x04

#define CMD_READ  0x10
#define CMD_WRITE 0x11

/*
 * Poll the command/status port until (status & mask) == val.
 *
 * Uses exponential backoff starting at 8 us, doubling at each attempt,
 * up to 24 tries (~100 ms worst case) to avoid hammering the LPC bus.
 *
 * Returns 0 as soon as the condition matches, -ETIMEDOUT otherwise.
 */
static int wait_status(unsigned char val, unsigned char mask)
{
	int us = 8;
	for (int i = 0; i < 24; i++) {
		if ((inb(CMD_PORT) & mask) == val)
			return 0;
		usleep(us);
		us <<= 1;
	}
	return -ETIMEDOUT;
}

/*
 * Send one byte to the given port after the required handshake:
 * first wait for the input buffer to be closed (ST_IB_CLOSED clear),
 * then for BUSY to be set, meaning the SMC is ready to accept data.
 *
 * Returns 0 on success, -EIO on handshake timeout.
 */
static int send_byte(unsigned char b, int port)
{
	if (wait_status(0, ST_IB_CLOSED))
		return -EIO;
	if (wait_status(ST_BUSY, ST_BUSY))
		return -EIO;
	outb(b, port);
	return 0;
}

/*
 * Write a command byte (CMD_READ/CMD_WRITE) to the command port once
 * the input buffer is closed and the SMC can accept it.
 *
 * Returns 0 on success, -EIO on timeout.
 */
static int send_command(unsigned char cmd)
{
	if (wait_status(0, ST_IB_CLOSED))
		return -EIO;
	outb(cmd, CMD_PORT);
	return 0;
}

/*
 * Make sure the SMC is idle (BUSY clear) before starting a transaction.
 *
 * If it stays busy past the first wait, a READ command is sent as a
 * recovery/flush attempt, then BUSY is waited on once more.
 *
 * Returns 0 if the SMC is idle, negative errno otherwise.
 */
static int smc_sane(void)
{
	int ret = wait_status(0, ST_BUSY);
	if (!ret)
		return 0;
	ret = send_command(CMD_READ);
	if (ret)
		return ret;
	return wait_status(0, ST_BUSY);
}

/*
 * Send the 4-character SMC key, one byte at a time through send_byte().
 *
 * Returns 0 on success, -EIO if any byte fails the handshake.
 */
static int send_argument(const char *key)
{
	for (int i = 0; i < 4; i++)
		if (send_byte((unsigned char)key[i], DATA_PORT))
			return -EIO;
	return 0;
}

/*
 * Read len bytes from the given SMC key into buf.
 *
 * Protocol sequence:
 *  1. make sure the SMC is not busy (smc_sane flushes it if needed);
 *  2. send the READ command, then the key (4 bytes) and the expected length;
 *  3. for each expected byte, poll the status until AWAITING_DATA|BUSY
 *     are set, then read from the data port;
 *  4. drain: flush any leftover queued bytes (up to 16 attempts) to bring
 *     the interface back to a coherent state;
 *  5. wait for processing to finish (BUSY cleared).
 *
 * Returns 0 on success, -EIO on error or timeout.
 */
static int read_smc(const char *key, unsigned char *buf, unsigned char len)
{
	if (smc_sane())
		return -EIO;
	if (send_command(CMD_READ) || send_argument(key))
		return -EIO;
	if (send_byte(len, DATA_PORT))
		return -EIO;
	for (int i = 0; i < len; i++) {
		if (wait_status(ST_AWAITING_DATA | ST_BUSY,
				ST_AWAITING_DATA | ST_BUSY))
			return -EIO;
		buf[i] = inb(DATA_PORT);
	}
	/* drain leftover bytes that were not requested, if any */
	for (int i = 0; i < 16; i++) {
		usleep(8);
		if (!(inb(CMD_PORT) & ST_AWAITING_DATA))
			break;
		inb(DATA_PORT);
	}
	return wait_status(0, ST_BUSY);
}

/*
 * Write len bytes from buf into the given SMC key.
 *
 * Protocol sequence:
 *  1. make sure the SMC is not busy (smc_sane flushes it if needed);
 *  2. send the WRITE command, then the key (4 bytes) and the payload length;
 *  3. send each payload byte, gated on the IB-closed/BUSY handshake done
 *     inside send_byte();
 *  4. wait for processing to finish (BUSY cleared).
 *
 * Returns 0 on success, -EIO on error or timeout.
 */
static int write_smc(const char *key, const unsigned char *buf, unsigned char len)
{
	if (smc_sane())
		return -EIO;
	if (send_command(CMD_WRITE) || send_argument(key))
		return -EIO;
	if (send_byte(len, DATA_PORT))
		return -EIO;
	for (int i = 0; i < len; i++)
		if (send_byte(buf[i], DATA_PORT))
			return -EIO;
	return wait_status(0, ST_BUSY);
}

/*
 * Entry point: parses the command line, acquires direct access to the SMC
 * I/O ports and dispatches to read_smc/write_smc.
 *
 * Usage:
 *   smc get KEY          read one byte from KEY and print it (dec + hex)
 *   smc set KEY VALUE    write VALUE into KEY, then verify by re-reading it;
 *                        a mismatch is reported with a warning
 *
 * KEY must be exactly 4 characters; VALUE is parsed with strtol() base 0,
 * so both decimal and hex notation (e.g. 0x28) are accepted.
 *
 * Requires root: port I/O access is granted through ioperm(), which is
 * always released before exiting.
 * Exit codes: 0 success, 1 runtime or permission error, 2 usage error.
 */
int main(int argc, char **argv)
{
	int is_set = (argc == 4 && strcmp(argv[1], "set") == 0);

	if (!is_set && !(argc == 3 && strcmp(argv[1], "get") == 0)) {
		fprintf(stderr, "uso: %s get CHIAVE | %s set CHIAVE VALORE\n",
			argv[0], argv[0]);
		return 2;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "serve root\n");
		return 1;
	}
	if (ioperm(DATA_PORT, NR_PORTS, 1)) {
		perror("ioperm");
		return 1;
	}

	const char *key = argv[2];
	if (strlen(key) != 4) {
		fprintf(stderr, "la chiave SMC deve essere di 4 caratteri\n");
		ioperm(DATA_PORT, NR_PORTS, 0);
		return 2;
	}

	int rc = 0;
	unsigned char buf[1] = {0};

	if (is_set) {
		char *end;
		long v = strtol(argv[3], &end, 0);
		if (*end || v < 0 || v > 100) {
			fprintf(stderr, "valore non valido (0-100)\n");
			ioperm(DATA_PORT, NR_PORTS, 0);
			return 2;
		}
		buf[0] = (unsigned char)v;
		rc = write_smc(key, buf, 1);
		if (rc) {
			fprintf(stderr, "scrittura fallita\n");
		} else if (read_smc(key, buf, 1)) {
			/* write succeeded but read-back verification failed */
			printf("scrittura ok ma verifica non riuscita\n");
		} else {
			printf("%s = %d (0x%02x)%s\n", key, buf[0], buf[0],
			       buf[0] == (unsigned char)v ? "" :
			       " [ATTENZIONE: il valore letto e' diverso]");
		}
	} else {
		rc = read_smc(key, buf, 1);
		if (rc)
			fprintf(stderr, "lettura fallita\n");
		else
			printf("%s = %d (0x%02x)\n", key, buf[0], buf[0]);
	}

	ioperm(DATA_PORT, NR_PORTS, 0);
	return rc ? 1 : 0;
}
