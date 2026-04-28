# Makefile for Tiny UDP Socket test suite

CC       = gcc
CFLAGS   = -Wall -Wextra -std=c99
INC      = -I../src
SRC_DIR  = ../src

.PHONY: all clean baremetal rtos

all: baremetal rtos

baremetal: test_udp_stack
	./test_udp_stack

test_udp_stack: test_udp_stack.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_stack.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_udp_stack.c $(SRC_DIR)/udp_stack.c

rtos: test_rtos_udp_stack
	./test_rtos_udp_stack

test_rtos_udp_stack: test_rtos_udp_stack.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_stack.h $(SRC_DIR)/udp_rtos_stub.c $(SRC_DIR)/udp_rtos_stub.h
	$(CC) $(CFLAGS) $(INC) -DUDP_RTOS -pthread -o $@ test_rtos_udp_stack.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_rtos_stub.c

clean:
	rm -f test_udp_stack test_rtos_udp_stack
