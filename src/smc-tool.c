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
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/io.h>

#define DATA_PORT 0x300
#define CMD_PORT  0x304
#define NR_PORTS  32

#define ST_AWAITING_DATA 0x01
#define ST_IB_CLOSED     0x02
#define ST_BUSY          0x04

#define CMD_READ              0x10
#define CMD_WRITE             0x11
#define CMD_GET_KEY_BY_INDEX  0x12
#define CMD_READ_TYPE         0x13

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
 * Query the SMC type descriptor of the given key through the
 * GET_KEY_TYPE command (CMD_READ_TYPE).
 *
 * Protocol sequence:
 *  1. make sure the SMC is not busy;
 *  2. send the GET_KEY_TYPE command, then the key (4 bytes);
 *  3. read back the 6-byte response:
 *       - byte 0:     length in bytes of the key data;
 *       - bytes 1-4:  ASCII type string (e.g. "ui8 ", "fpe2", "sp78");
 *       - byte 5:     key flags;
 *  4. wait for processing to finish (BUSY cleared).
 *
 * On success *len holds the data size and type receives the
 * null-terminated 4-character type string (type must be at least
 * 5 bytes). The flags byte is consumed but not returned.
 *
 * Returns 0 on success, -EIO on error or timeout.
 */
static int smc_get_key_type(const char *key, unsigned char *len, char *type)
{
	unsigned char buf[6];
	if (smc_sane()){
		return -EIO;
	}
	if (send_command(CMD_READ_TYPE)){
		return -EIO;
	}
	if (send_argument(key)){
		return -EIO;
	}

	for (int i = 0; i < 6; i++){
		if (wait_status(ST_AWAITING_DATA | ST_BUSY,
				ST_AWAITING_DATA | ST_BUSY))
			return -EIO;
		buf[i] = inb(DATA_PORT);	
	}
	*len = buf[0];
	memcpy(type, buf + 1, 4);
	type[4] = '\0';
	return wait_status(0, ST_BUSY);
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
 * Read the total number of public SMC keys from the "#KEY" register.
 *
 * "#KEY" is a read-only ui32 key whose value is the count of keys
 * in the SMC key index. The 4-byte response is big-endian.
 *
 * On success *count receives the number of keys.
 * Returns 0 on success, -EIO on error or timeout.
 */
static int smc_read_key_count(unsigned int *count)
{
	unsigned char buf[4];
	int ret;

	ret = read_smc("#KEY", buf, 4);
	if (ret)
		return ret;

  /*
   * Converts 4 bytes from big-endian (SMC byte order) to a 32-bit
   * unsigned integer in host byte order (little-endian on x86).
   *  buf[0] = 0x00    ← most significant byte
   *  buf[1] = 0x00
   *  buf[2] = 0x01
   *  buf[3] = 0x3A    ← least significant byte
   */
	*count = ((unsigned int)buf[0] << 24) |
		 ((unsigned int)buf[1] << 16) |
		 ((unsigned int)buf[2] << 8)  |
		 (unsigned int)buf[3];
	return 0;
}

/*
 * Read the 4-character key name at the given index in the SMC key index.
 *
 * Protocol sequence:
 *  1. smc_sane();
 *  2. send CMD_GET_KEY_BY_INDEX (0x12);
 *  3. send the index as 4 big-endian bytes;
 *  4. read back 4 bytes (the key name) from the data port;
 *  5. wait for processing to finish (BUSY cleared).
 *
 * On success key receives the null-terminated 4-character key name
 * (key must be at least 5 bytes).
 * Returns 0 on success, -EIO on error or timeout.
 */
static int smc_get_key_by_index(unsigned int index, char *key)
{
	unsigned char idx[4];
	unsigned char buf[4];

	idx[0] = (index >> 24) & 0xff;
	idx[1] = (index >> 16) & 0xff;
	idx[2] = (index >> 8)  & 0xff;
	idx[3] = index & 0xff;

	if (smc_sane())
		return -EIO;
	if (send_command(CMD_GET_KEY_BY_INDEX))
		return -EIO;
	for (int i = 0; i < 4; i++)
		if (send_byte(idx[i], DATA_PORT))
			return -EIO;

	for (int i = 0; i < 4; i++) {
		if (wait_status(ST_AWAITING_DATA | ST_BUSY,
				ST_AWAITING_DATA | ST_BUSY))
			return -EIO;
		buf[i] = inb(DATA_PORT);
	}

	memcpy(key, buf, 4);
	key[4] = '\0';
	return wait_status(0, ST_BUSY);
}

/*
 * Enumerate all public SMC keys and print their details.
 *
 * For each key in the index:
 *  1. read the key name by index (CMD_GET_KEY_BY_INDEX);
 *  2. query the type descriptor (CMD_READ_TYPE) for type, length and flags;
 *  3. print the result.
 *
 * Returns 0 on success, negative errno on error.
 */
static int smc_list_keys(void)
{
	unsigned int count = 0;
	int ret;

	ret = smc_read_key_count(&count);
	if (ret)
		return ret;

	printf("Key count: %u\n", count);

	for (unsigned int i = 0; i < count; i++) {
		char key[5] = {0};
		unsigned char len = 0;
		char type[5] = "?";

		ret = smc_get_key_by_index(i, key);
		if (ret) {
			fprintf(stderr, "Failed to read key at index %u\n", i);
			continue;
		}

		ret = smc_get_key_type(key, &len, type);
		if (ret)
			printf("[%s] unknown type, len unknown\n", key);
		else
			printf("[%s] type [%s] len [%u]\n", key, type, len);
	}

	return 0;
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
 * Print the program usage to the given stream: stdout when help is
 * explicitly requested (--help), stderr on a usage error. A single copy
 * of the text keeps the two output paths in sync.
 */
static void usage(FILE *out)
{
	fprintf(out, "Usage: smc [OPTIONS] COMMAND\n");
	fprintf(out, "\n");
	fprintf(out, "Read and write Apple SMC keys directly from userspace (Linux on Intel Macs, requires root)\n");
	fprintf(out, "Commands:\nget KEY\tread one byte from KEY\nset KEY VALUE\twrite VALUE (0-100) into KEY and verify by re-reading\nlist\tenumerate all keys in the SMC key index\n");
	fprintf(out, "Options:\n\t-h, --help\tshow this help and exit\n-V, --version\tshow version information and exit\n");
	fprintf(out, "Key is a 4-character SMC key (e.g. F0Mn). VALUE accepts decimal or hex (e.g. 40 or 0x28).\n");
	fprintf(out, "\n");
	fprintf(out, "Exit status:\n\t0\tsuccess\n\t1\truntime or permission error\n\t2\tusage error\n");
	fprintf(out, "Warning: writing incorrect SMC keys may destabilize your machine.\n");
	fprintf(out, "Examples:\n\tsudo smc list\n\tsudo smc get F0Mn\n\tsudo smc set F0Mn 40\n");
}

/*
 * Entry point: parses global options (-h/--help, -V/--version), dispatches
 * to the get/set subcommands and acquires direct access to the SMC I/O
 * ports through ioperm().
 *
 * Usage:
 *   smc --help           print the full usage on stdout and exit
 *   smc --version        print the program version and exit
 *   smc list             enumerate all keys in the SMC key index
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
	int is_list = (argc == 2 && strcmp(argv[1], "list") == 0);
	bool is_argc = argc > 1;

	if (is_argc && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))){
		usage(stdout);
		return 0;
	}
	if (is_argc && (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version"))){
		printf("smc-tool %s\n", VERSION);
		return 0;
	}
	if (geteuid() != 0){
		fprintf(stderr, "Need root\n");
		return 1;
	}
	if (ioperm(DATA_PORT, NR_PORTS, 1)){
		perror("ioperm");
		return 1;
	}

	if (is_list){
		int rc = smc_list_keys();
		ioperm(DATA_PORT, NR_PORTS, 0);
		return rc ? 1 : 0;
	}

	if (!is_set && !(argc == 3 && strcmp(argv[1], "get") == 0)){
		ioperm(DATA_PORT, NR_PORTS, 0);
		goto usage_error;
	}

	const char *key = argv[2];
	if (strlen(key) != 4){
		fprintf(stderr, "The SMC key must be exactly 4 characters\n");
		ioperm(DATA_PORT, NR_PORTS, 0);
		return 2;
	}
	int rc = 0;
	unsigned char buf[32] = {0};
	unsigned char len = 1;
	char type[5] = "?";
	
	/*Here call new function with fallback*/
	if (smc_get_key_type(key, &len, type) || len > sizeof(buf)){
		len = 1;
		fprintf(stderr, "Warning: cannot read key type, assuming ui8/1 byte\n");
	}else{
		printf("Length: %d\n", len);
		printf("Type: %s\n", type);
	}
	if (is_set){
		char *end;
		long v = strtol(argv[3], &end, 0);
		if (*end || v < 0 || v > 100) {
			fprintf(stderr, "Invalid value (0-100)\n");
			ioperm(DATA_PORT, NR_PORTS, 0);
			return 2;
		}
		buf[0] = (unsigned char)v;
		rc = write_smc(key, buf, 1);
		if (rc){
			fprintf(stderr, "Write failed\n");
		} else if (read_smc(key, buf, len)) {
			/* write succeeded but read-back verification failed */
			printf("Write ok, but not verified\n");
		} else{
			printf("%s = %d (0x%02x)%s\n", key, buf[0], buf[0],
			       buf[0] == (unsigned char)v ? "" :
			       " [ALERT: read value not equal]");
		}
	} else{
		rc = read_smc(key, buf, len);
		if (rc)
			fprintf(stderr, "Read failed\n");
		else
			printf("%s = %d (0x%02x)\n", key, buf[0], buf[0]);
	}

	ioperm(DATA_PORT, NR_PORTS, 0);
	return rc ? 1 : 0;

usage_error:
	fprintf(stderr, "Try 'smc --help' for more information\n");
	return 2;
}
