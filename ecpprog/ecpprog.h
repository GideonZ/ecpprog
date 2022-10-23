#ifndef __ECPPROG_H__
#define __ECPPROG_H__

#include <stdio.h>
#include <stdbool.h>

uint32_t read_idcode();
void ecp_prog_sram(FILE *f, bool verbose);
void ecp_prog_flash(FILE *f, bool disable_protect, bool dont_erase, bool bulk_erase, bool erase_mode, int erase_block_size, int rw_offset);
int  ecp_flash_verify(FILE *f, int rw_offset);
void ecp_init_flash_mode();

#endif