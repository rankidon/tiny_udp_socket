# Makefile for Tiny UDP Socket test suite

CC       = gcc
CFLAGS   = -Wall -Wextra -std=c11
INC      = -I../src
SRC_DIR  = ../src

# wolfSSL (Homebrew, 托管环境测试用)
WOLFSSL_INC = -I/opt/homebrew/include
WOLFSSL_LIB = -L/opt/homebrew/lib -lwolfssl

# wolfSSL bare-metal source build
WOLFSSL_SRC_DIR  = ../wolfssl-5.9.1
WOLFSSL_BM_DIR   = ../build/wolfssl_bm
WOLFSSL_BM_LIB   = $(WOLFSSL_BM_DIR)/libwolfssl_bm.a
WOLFSSL_BM_CFLAGS = -DWOLFSSL_USER_SETTINGS -I$(WOLFSSL_SRC_DIR) -I.

.PHONY: all clean baremetal rtos wolfssl-types wolfssl-dtls wolfssl-dtls-baremetal

all: baremetal rtos wolfssl-types wolfssl-dtls

# ---- Core stack tests ----

baremetal: test_udp_stack
	./test_udp_stack

test_udp_stack: test_udp_stack.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_stack.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_udp_stack.c $(SRC_DIR)/udp_stack.c

rtos: test_rtos_udp_stack
	./test_rtos_udp_stack

test_rtos_udp_stack: test_rtos_udp_stack.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_stack.h $(SRC_DIR)/udp_rtos_stub.c $(SRC_DIR)/udp_rtos_stub.h
	$(CC) $(CFLAGS) $(INC) -DUDP_RTOS -pthread -o $@ test_rtos_udp_stack.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_rtos_stub.c

# ---- wolfSSL integration tests ----

wolfssl-types: test_wolfssl_types
	./test_wolfssl_types

test_wolfssl_types: test_wolfssl_types.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_stack.h
	$(CC) $(CFLAGS) $(INC) -o $@ test_wolfssl_types.c $(SRC_DIR)/udp_stack.c

wolfssl-dtls: test_wolfssl_dtls
	./test_wolfssl_dtls

test_wolfssl_dtls: test_wolfssl_dtls.c server-cert.pem server-key.pem
	$(CC) $(CFLAGS) $(WOLFSSL_INC) -o $@ test_wolfssl_dtls.c $(WOLFSSL_LIB)

# ---- wolfSSL DTLS bare-metal (自定义编译 wolfSSL，无 OS / 无文件系统) ----

wolfssl-dtls-baremetal: test_wolfssl_dtls_bm
	./test_wolfssl_dtls_bm

test_wolfssl_dtls_bm: test_wolfssl_dtls_baremetal.c $(SRC_DIR)/udp_stack.c $(SRC_DIR)/udp_stack.h \
		$(WOLFSSL_BM_LIB) server-cert.pem server-key.pem
	$(CC) $(CFLAGS) $(INC) $(WOLFSSL_BM_CFLAGS) -o $@ \
		test_wolfssl_dtls_baremetal.c $(SRC_DIR)/udp_stack.c $(WOLFSSL_BM_LIB)

# 编译 wolfSSL 裸机静态库（所有 .c 文件）
$(WOLFSSL_BM_LIB):
	@echo "Building wolfSSL bare-metal static library..."
	@mkdir -p $(WOLFSSL_BM_DIR)
	@for f in $(WOLFSSL_SRC_DIR)/src/*.c $(WOLFSSL_SRC_DIR)/wolfcrypt/src/*.c; do \
		[ -f "$$f" ] || continue; \
		obj="$(WOLFSSL_BM_DIR)/$$(basename $${f%.c}.o)"; \
		$(CC) -std=c11 -Wall -DWOLFSSL_USER_SETTINGS -I$(WOLFSSL_SRC_DIR) -I. \
			-c "$$f" -o "$$obj" 2>/dev/null || true; \
	done
	@ar rcs $(WOLFSSL_BM_LIB) $(WOLFSSL_BM_DIR)/*.o
	@echo "  -> $(WOLFSSL_BM_LIB)"

# ---- Test certificates (self-signed ECC) ----

server-cert.pem server-key.pem:
	@echo "Generating self-signed ECC cert for wolfSSL DTLS test..."
	openssl ecparam -name prime256v1 -genkey -noout -out server-key.pem
	openssl req -new -x509 -key server-key.pem -out server-cert.pem -days 30 \
		-subj "/C=US/O=TinyUDP/CN=localhost"
	cp server-cert.pem ca-cert.pem

clean:
	rm -f test_udp_stack test_rtos_udp_stack test_wolfssl_types test_wolfssl_dtls test_wolfssl_dtls_bm
	rm -f server-cert.pem server-key.pem ca-cert.pem
	rm -rf $(WOLFSSL_BM_DIR)
