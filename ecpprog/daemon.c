
#include "daemon.h"
#include "u2p_stuff.h"
#include "ecpprog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

#define BUF_SIZE 1024

#define USER_READ_ID 1
#define USER_READ_MEMORY 2 // (UINT32_T ADDRESS, INT WORDS, UINT8_T *DEST);
#define USER_WRITE_MEMORY 3 // (UINT32_T ADDRESS, INT WORDS, UINT32_T *SRC);
#define USER_READ_IO_REGISTERS 4 // 4 (UINT32_T ADDRESS, INT COUNT, UINT8_T *DATA);
#define USER_WRITE_IO_REGISTERS 5 // 5 (UINT32_T ADDRESS, INT COUNT, UINT8_T *DATA);
#define USER_SET_IO 6 // (INT VALUE);
#define USER_READ_CONSOLE 7 // (CHAR *DATA, INT BYTES);
#define USER_READ_CONSOLE2 11 // (CHAR *DATA, INT BYTES);
#define USER_UPLOAD 8 // (CONST CHAR *FILENAME, CONST UINT32_T DEST_ADDR);
#define USER_RUN_APPL 9 // (UINT32_T RUNADDR); 
#define USER_READ_DEBUG 10 // void
#define ECP_LOADFPGA 0x21 // (CONST CHAR *FILENAME, CONST UINT32_T DUMMY);
#define ECP_PROGFLASH 0x22 // (CONST CHAR *FILENAME, CONST UINT32_T DEST_ADDR);
#define ECP_READID 0x23 // void
#define ECP_UNIQUE_ID 0x24 // void
#define ECP_CLEARFPGA 0x25 // void
#define DAEMON_ID 0x41 // This is like an inquiry to see if the daemon is running.
#define SYNC_BYTE 0xCC
#define CODE_OKAY 0x00
#define CODE_BAD_SYNC 0xEE
#define CODE_UNKNOWN_COMMAND 0xED
#define CODE_BAD_PARAMS 0xEC
#define CODE_FILE_NOT_FOUND 0xEB
#define CODE_VERIFY_ERROR 0xEA
#define CODE_FIFO_ERROR 0xE9
#define CODE_PROGRESS 0xBF

int clientfd;
unsigned int pagediv;

void progress(void)
{
    pagediv ++;
    if (pagediv >= 4) {
        uint8_t update[2] = { CODE_PROGRESS, ECP_PROGFLASH };
        send(clientfd, update, 2, 0);    
        pagediv = 0;
    }
}

int start_daemon(int portnr)
{
	int lSocket;
	struct sockaddr_in sLocalAddr;

	lSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (lSocket < 0) {
		puts("lSocket < 0");
		return lSocket;
	}

	memset((char *)&sLocalAddr, 0, sizeof(sLocalAddr));
	sLocalAddr.sin_family = AF_INET;
//	sLocalAddr.sin_len = sizeof(sLocalAddr);
	sLocalAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int bind_ret;
    for(int i=0;i<20;i++) {
        printf("Bind attempt to port number %d\n", portnr);
        sLocalAddr.sin_port = htons(portnr);
        bind_ret = bind(lSocket, (struct sockaddr *)&sLocalAddr, sizeof(sLocalAddr));
        if (bind_ret == 0) {
            break;
        }
        portnr++;
    }

    if (bind_ret < 0) {
        close(lSocket);
        puts("bind failed");
        return bind_ret;
    }

    int listen_ret = listen(lSocket, 20);
	if ( listen_ret != 0 ) {
		close(lSocket);
		puts("listen failed");
		return listen_ret;
	}

    struct sockaddr_in client_addr;
    int addrlen=sizeof(client_addr);
    uint8_t buffer[BUF_SIZE];
    int nbytes;
    uint32_t *pul, addr;
    uint64_t *p64;
    int *psi, len, total;
    uint8_t *mem;

    printf("Listening to port %d\n", portnr);
	while (1) {
		clientfd = accept(lSocket, (struct sockaddr*)&client_addr, (socklen_t *)&addrlen);
		if (clientfd > 0) {
			puts("Accepted Connection");

            while(1) {
                printf("Waiting for command... ");
                fflush(stdout);
                nbytes = recv(clientfd, buffer, 2, MSG_WAITALL);

                if (nbytes < 2) {
                    printf("Client hung up.\n");
                    close(clientfd);
                    break;
                }
                printf("%02x %02x\n", buffer[0], buffer[1]);

                if (buffer[0] != SYNC_BYTE) {
                    buffer[1] = buffer[0];
                    buffer[0] = CODE_BAD_SYNC;
                    send(clientfd, buffer, 2, 0);
                    close(clientfd);
                    break;
                }

                uint8_t err = 0;
                switch(buffer[1]) {
                    case DAEMON_ID:
                        buffer[2] = 0x01; // version
                        buffer[3] = 0x00;
                        buffer[0] = CODE_OKAY;
                        send(clientfd, buffer, 4, 0);
                        break;
                    case USER_READ_ID:
                        pul = (uint32_t *)(buffer+4);
                        *pul = user_read_id();
                        buffer[0] = CODE_OKAY;
                        buffer[2] = 0; // padding
                        buffer[3] = 0; // padding
                        send(clientfd, buffer, 8, 0);
                        break;
                    case USER_READ_DEBUG:
                        pul = (uint32_t *)(buffer+4);
                        *pul = user_read_debug();
                        buffer[0] = CODE_OKAY;
                        buffer[2] = 0; // padding
                        buffer[3] = 0; // padding
                        send(clientfd, buffer, 8, 0);
                        break;
                    case USER_READ_MEMORY:
                        // (UINT32_T ADDRESS, INT WORDS, UINT8_T *DEST);
                        nbytes = recv(clientfd, buffer+4, 8, MSG_WAITALL);
                        if (nbytes != 8) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            pul = (uint32_t *)(buffer+4);
                            psi = (int *)(buffer+8);
                            len = *psi;
                            if (len < 1024*1024) {
                                mem = malloc(4*len);
                                int fifo = user_read_memory(*pul, len, mem);
                                if (fifo != 4*len) {
                                    err = CODE_FIFO_ERROR;
                                    buffer[0] = err;
                                } else {
                                    buffer[0] = CODE_OKAY;
                                }
                                send(clientfd, buffer, 2, 0);
                                send(clientfd, mem, 4*len, 0); // Might be bogus
                                free(mem);
                            } else {
                                err = CODE_BAD_PARAMS;
                            }
                        }
                        break;
                    case USER_WRITE_MEMORY:
                        // (UINT32_T ADDRESS, INT WORDS, UINT8_T *SRC);
                        nbytes = recv(clientfd, buffer+4, 8, MSG_WAITALL);
                        if (nbytes != 8) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            pul = (uint32_t *)(buffer+4);
                            psi = (int *)(buffer+8);
                            len = *psi;
                            if (len < 1024*1024) {
                                mem = malloc(4*len);
                                printf("Reading %d bytes from socket.\n", len);
                                nbytes = recv(clientfd, mem, len ,MSG_WAITALL);
                                printf("Got %d bytes from socket!\n", nbytes);
                                user_write_memory(*pul, len, (uint32_t*)mem);
                                buffer[0] = CODE_OKAY;
                                send(clientfd, buffer, 2, 0);
                                free(mem);
                            } else {
                                err = CODE_BAD_PARAMS;
                            }
                        }
                        break;
                    case USER_READ_IO_REGISTERS:
                        // 4 (UINT32_T ADDRESS, INT COUNT, UINT8_T *DATA);
                        // (UINT32_T ADDRESS, INT WORDS, UINT8_T *DEST);
                        nbytes = recv(clientfd, buffer+4, 8, MSG_WAITALL);
                        if (nbytes != 8) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            pul = (uint32_t *)(buffer+4);
                            psi = (int *)(buffer+8);
                            addr = *pul;
                            len = *psi;
                            if (len < 256) {
                                int got = user_read_io_registers(addr, len, buffer+2);
                                if (got != len) {
                                    buffer[0] = CODE_FIFO_ERROR;
                                    err = CODE_FIFO_ERROR;
                                } else {
                                    buffer[0] = CODE_OKAY;
                                }
                                send(clientfd, buffer, 2 + len, 0);
                            } else {
                                err = CODE_BAD_PARAMS;
                            }
                        }
                        break;
                    case USER_WRITE_IO_REGISTERS:
                        // (UINT32_T ADDRESS, INT WORDS, UINT8_T *SRC);
                        nbytes = recv(clientfd, buffer+4, 8, MSG_WAITALL);
                        if (nbytes != 8) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            pul = (uint32_t *)(buffer+4);
                            psi = (int *)(buffer+8);
                            addr = *pul;
                            len = *psi;
                            if (len < 256) {
                                printf("Reading %d bytes from socket.\n", len);
                                nbytes = recv(clientfd, buffer+16, len ,MSG_WAITALL);
                                printf("Got %d bytes from socket!\n", nbytes);
                                user_write_io_registers(addr, len, buffer+16);
                                buffer[0] = CODE_OKAY;
                                send(clientfd, buffer, 2, 0);
                            } else {
                                err = CODE_BAD_PARAMS;
                            }
                        }
                        break;
                    case USER_SET_IO:
                        nbytes = recv(clientfd, buffer+4, 4, MSG_WAITALL);
                        if (nbytes != 4) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            psi = (int *)(buffer+4);
                            user_set_io(*psi);
                            buffer[0] = CODE_OKAY;
                            send(clientfd, buffer, 2, 0);
                        }
                        break;
                    case USER_READ_CONSOLE:
                        // can read max BUF_SIZE-5, due to terminating null and four return bytes
                        len = BUF_SIZE-5;
                        mem  = buffer+4;
                        total = 0;
                        do {
                            nbytes = user_read_console((char *)mem, len);
                            total += nbytes;
                            len -= nbytes;
                            mem += nbytes;
                        } while(nbytes);
                        buffer[0] = CODE_OKAY;
                        *((uint16_t*)(buffer + 2)) = (uint16_t)total;
                        send(clientfd, buffer, total+4, 0);
                        break;
                    case USER_READ_CONSOLE2:
                        // can read max BUF_SIZE-5, due to terminating null and four return bytes
                        len = BUF_SIZE-5;
                        mem  = buffer+4;
                        total = 0;
                        do {
                            nbytes = user_read_console2((char *)mem, len);
                            total += nbytes;
                            len -= nbytes;
                            mem += nbytes;
                        } while(nbytes);
                        buffer[0] = CODE_OKAY;
                        *((uint16_t*)(buffer + 2)) = (uint16_t)total;
                        send(clientfd, buffer, total+4, 0);
                        break;
                    case USER_UPLOAD:
                        // (CONST CHAR *FILENAME, CONST UINT32_T DEST_ADDR);
                        // destination address first, and then length of string and the string itself
                        nbytes = recv(clientfd, buffer+4, 5, MSG_WAITALL);
                        pul = (uint32_t *)(buffer+4);
                        addr = *pul;
                        len = (int)buffer[8];

                        if ((nbytes != 5) || !len) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            nbytes = recv(clientfd, buffer+16, len, MSG_WAITALL);
                            if (nbytes != len) {
                                err = CODE_BAD_PARAMS;
                            } else {
                                buffer[16+len] = 0; // make sure it's 0 terminated
                                user_upload((char *)buffer + 16, addr);
                                buffer[0] = CODE_OKAY;
                                send(clientfd, buffer, 2, 0);
                            }
                        }
                        break;
                    case USER_RUN_APPL:
                        nbytes = recv(clientfd, buffer+4, 4, MSG_WAITALL);
                        if (nbytes != 4) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            pul = (uint32_t *)(buffer+4);
                            user_run_appl(*pul);
                            buffer[0] = CODE_OKAY;
                            send(clientfd, buffer, 2, 0);
                        }
                        break;
                    case ECP_READID:
                        pul = (uint32_t *)(buffer+4);
                        *pul = read_idcode();
                        buffer[0] = CODE_OKAY;
                        buffer[2] = 0; // padding
                        buffer[3] = 0; // padding
                        send(clientfd, buffer, 8, 0);
                        break;
                    case ECP_UNIQUE_ID:
                        p64 = (uint64_t *)(buffer+4);
                        *p64 = read_unique_id();
                        buffer[0] = CODE_OKAY;
                        buffer[2] = 0; // padding
                        buffer[3] = 0; // padding
                        send(clientfd, buffer, 12, 0);
                        break;
                    case ECP_CLEARFPGA:
                        ecp_init_flash_mode();
                        buffer[0] = CODE_OKAY;
                        send(clientfd, buffer, 2, 0);
                        break;
                    case ECP_LOADFPGA:
                    case ECP_PROGFLASH:
                        // (CONST CHAR *FILENAME);
                        // offset address first, and then length of string and the string itself
                        nbytes = recv(clientfd, buffer+4, 5, MSG_WAITALL);
                        pul = (uint32_t *)(buffer+4);
                        addr = *pul;
                        len = (int)buffer[8];

                        if ((nbytes != 5) || !len) {
                            err = CODE_BAD_PARAMS;
                        } else {
                            nbytes = recv(clientfd, buffer+16, len, MSG_WAITALL);
                            if (nbytes != len) {
                                err = CODE_BAD_PARAMS;
                                break;
                            } 
                            buffer[16+len] = 0; // make sure it's 0 terminated
                            FILE *f = fopen((char *)buffer + 16, "rb");
                            if (!f) {
                                err = CODE_FILE_NOT_FOUND;
                                break;
                            }
                            buffer[0] = CODE_OKAY;

                            if(buffer[1] == ECP_LOADFPGA) {
                                ecp_prog_sram(f, false);
                            } else {
                                ecp_init_flash_mode();
                                pagediv = 0;
                                ecp_prog_flash(f, true, false, false, false, 64, (int)addr, &progress);
                                if (ecp_flash_verify(f, (int)addr)) {
                                    buffer[0] = CODE_VERIFY_ERROR;
                                }
                            }
                            fclose(f);
                            send(clientfd, buffer, 2, 0);
                        }
                        break;
                    default:
                        buffer[0] = CODE_UNKNOWN_COMMAND;
                        send(clientfd, buffer, 2, 0);
                        close(clientfd);
                        break;
                }
                if (err) {
                    buffer[0] = err;
                    send(clientfd, buffer, 2, 0);
                    close(clientfd);
                    break;
                }
            }
		}
	}
	close(lSocket);
	puts("Socked closed.");
    return 0;
}
