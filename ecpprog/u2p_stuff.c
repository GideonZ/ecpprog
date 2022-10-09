/*
 *  ecpprog -- simple programming tool for FTDI-based JTAG programmers
 *  Based on iceprog
 *
 *  Copyright (C) 2015  Clifford Wolf <clifford@clifford.at>
 *  Copyright (C) 2018  Piotr Esden-Tempski <piotr@esden.net>
 *  Copyright (C) 2020  Gregory Davill <greg.davill@gmail.com>
 *  Copyright (C) 2022  Gideon Zweijtzer <gideon.zweijtzer@gmail.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 *  Relevant Documents:
 *  -------------------
 *  http://www.latticesemi.com/~/media/Documents/UserManuals/EI/icestickusermanual.pdf
 *  http://www.micron.com/~/media/documents/products/data-sheet/nor-flash/serial-nor/n25q/n25q_32mb_3v_65nm.pdf
 *
 *  This module is an extension on ecpprog, and is created to talk with a custom JTAG block
 *  on the ECP5. This block allows access to memory and I/O, and is capable of booting a CPU
 *  to run code that is uploaded through this interface.
 *
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h> /* _setmode() */
#include <fcntl.h> /* _O_BINARY */
#endif

#include "jtag.h"
#include "dump_hex.h"

#define	LSC_USER1 0x32
#define	LSC_USER2 0x38


static void set_user_ir(uint8_t ir)
{
	uint8_t data[1] = { LSC_USER1 };
	jtag_go_to_state(STATE_SHIFT_IR);
	jtag_tap_shift(data, data, 8, true);

	data[0] = ir | (ir << 4);
	//printf("Writing IR to %02x ", ir);
	jtag_go_to_state(STATE_SHIFT_DR);
	jtag_tap_shift(data, data, 8, true);
	//printf("Got %02x\n", data[0]);
	jtag_go_to_state(STATE_RUN_TEST_IDLE);
}

static void rw_user_data(uint8_t *data, int bits)
{
	uint8_t inst[1];
	inst[0] = LSC_USER2;
	jtag_go_to_state(STATE_SHIFT_IR);
	jtag_tap_shift(inst, inst, 8, true);

	jtag_go_to_state(STATE_SHIFT_DR);
	if(bits) {
		jtag_tap_shift(data, data, bits, true);
		jtag_go_to_state(STATE_RUN_TEST_IDLE);
	}
}

static int read_fifo(uint8_t *data, int bytes)
{
	uint8_t available = 0;
	int bytes_read = 0;
	while(bytes) {
		// set_user_ir(3);
		// uint32_t dbg;
		// rw_user_data((uint8_t *)&dbg, 32);
		// printf("DBG = %08x\n", dbg);

		set_user_ir(4);
		rw_user_data(NULL, 0); // set USER2 as IR, and go do SHIFT_DR state

		available = 0; // to make sure TDI = 0
		jtag_tap_shift(&available, &available, 8, false);
		// printf("Number of bytes available in FIFO: %d\n", available);
		if (available > bytes) {
			available = (uint8_t)bytes;
		}
		if (!available) {
			printf("No more bytes in fifo?!\n");
			break;
		}

		memset(data, 0, available);
		data[available-1] = 0xF0; // no read on last
		jtag_tap_shift(data, data, 8*available, true);

		bytes -= available;
		data += available;
		bytes_read += available;
	}
	return bytes_read;
}

void user_read_memory(uint32_t address, int words, uint8_t *dest)
{
	while(words > 0) {
		int now = (words > 256) ? 256 : words;
		uint32_t caddr = address;
		uint8_t read_cmd[] = { 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x00, 0x03 };
		read_cmd[0] = (uint8_t)caddr;  caddr >>= 8;
		read_cmd[2] = (uint8_t)caddr;  caddr >>= 8;
		read_cmd[4] = (uint8_t)caddr;  caddr >>= 8;
		read_cmd[6] = (uint8_t)caddr;
		read_cmd[8] = (uint8_t)(now - 1);
		set_user_ir(5);
		rw_user_data(read_cmd, 80);
		if (!read_fifo(dest, 4*now))
			break;
		address += 4*now;
		dest += 4*now;
		words -= now;
	}
	jtag_go_to_state(STATE_RUN_TEST_IDLE);
}

void user_write_memory(uint32_t address, int words, uint32_t *src)
{
	uint8_t read_cmd[] = { 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x07, 0x80, 0x01 };
	read_cmd[0] = (uint8_t)address;  address >>= 8;
	read_cmd[2] = (uint8_t)address;  address >>= 8;
	read_cmd[4] = (uint8_t)address;  address >>= 8;
	read_cmd[6] = (uint8_t)address;
	set_user_ir(5);
	rw_user_data(read_cmd, 80);
	set_user_ir(6);
	rw_user_data((uint8_t *)src, words * 32);
	jtag_go_to_state(STATE_RUN_TEST_IDLE);
}

void user_read_io_registers(uint32_t address, int count, uint8_t *data)
{
	uint8_t read_cmd[] = { 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x0D };
	read_cmd[0] = (uint8_t)address;  address >>= 8;
	read_cmd[2] = (uint8_t)address;  address >>= 8;
	read_cmd[4] = (uint8_t)address;  address >>= 8;
	set_user_ir(5);
	rw_user_data(read_cmd, 48); // first 6 bytes, return to idle
	rw_user_data(read_cmd, 0); // set to data state again, do not return to idle
	for(int i=0; i < count; i++) {
		jtag_tap_shift(read_cmd + 6, read_cmd, 16, false); // overwrites the first two bytes of read_cmd, sends as many read commands as requested
	}
	jtag_go_to_state(STATE_RUN_TEST_IDLE);
	// Now read all the data from the fifo.
	read_fifo(data, count);
}

void user_write_io_registers(uint32_t address, int count, uint8_t *data)
{
	uint8_t read_cmd[] = { 0x00, 0x04, 0x00, 0x05, 0x00, 0x06, 0x00, 0x0F };
	read_cmd[0] = (uint8_t)address;  address >>= 8;
	read_cmd[2] = (uint8_t)address;  address >>= 8;
	read_cmd[4] = (uint8_t)address;  address >>= 8;
	set_user_ir(5);
	rw_user_data(read_cmd, 48); // first 6 bytes
	rw_user_data(read_cmd, 0); // set to data state again
	for(int i=0; i < count; i++) {
		read_cmd[6] = data[i];
		jtag_tap_shift(read_cmd + 6, read_cmd, 16, false); // overwrites the first two bytes of read_cmd
	}
	jtag_go_to_state(STATE_RUN_TEST_IDLE);
}

uint32_t user_read_id()
{
	uint32_t code = 0;
	set_user_ir(0);
	rw_user_data((uint8_t*)&code, 32);
	printf("Data read from USER JTAG: %08x\n", code);
    return code;
}

void user_test_jtag()
{
    if (user_read_id() != 0xdead1541) {
        printf("User ID fail. Stopped.\n");
        return;
    }

	FILE *f = fopen("u2p_ecp5.lpf", "rb");
    if (!f) {
        printf("Cannot open test file. Stopped.\n");
        return;
    }
	static uint32_t filedata[32768];
	int words = fread(filedata, 4, 32768, f);
	fclose(f);
	user_write_memory(0, words, filedata);

	memset(filedata, 0x55, 4*words);
	user_read_memory(0x00000, words, (uint8_t *)filedata);
	for (int i=0;i<75;i++) {
		printf("%2d: %08x\n", i, filedata[i]);
	}
	printf("Words = %d\n", words);
	f = fopen("readback", "wb");
	fwrite(filedata, 4, words, f);
	fclose(f);

	static uint8_t regs[32];
	user_read_io_registers(0, 32, regs);
	dump_hex_relative(regs, 32);

	const char *msg = "Haha!";
	for(int i=0; i<strlen(msg); i++) {
		user_write_io_registers(0x10, 1, (uint8_t *)&msg[i]);
	}
}

void user_set_io(int value)
{
	uint8_t data = (uint8_t)value;
	printf("Setting User Register to %02x\n", data);
	set_user_ir(2);
	rw_user_data(&data, 8);
	printf("Old data: %02x\n", data);

    if (value == -1) {
        user_test_jtag();
    }
}
