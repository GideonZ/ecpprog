#ifndef U2P_STUFF_H
#include <stdint.h>

uint32_t user_read_id();
void user_read_memory(uint32_t address, int words, uint8_t *dest);
void user_write_memory(uint32_t address, int words, uint32_t *src);
void user_read_io_registers(uint32_t address, int count, uint8_t *data);
void user_write_io_registers(uint32_t address, int count, uint8_t *data);
void user_test_jtag();
void user_set_io(int value);

#endif
