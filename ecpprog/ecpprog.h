#ifndef __ECPPROG_H__
#define __ECPPROG_H__

#include <stdio.h>
#include <stdbool.h>

typedef void (*callback_t)(void);
uint32_t read_idcode();
uint64_t read_unique_id();
void ecp_prog_sram(FILE *f, bool verbose);
int ecp_prog_flash(FILE *f, bool disable_protect, bool dont_erase, bool bulk_erase, bool erase_mode, int erase_block_size, int rw_offset, callback_t cb);
int  ecp_flash_verify(FILE *f, int rw_offset);
void ecp_init_flash_mode();

#endif