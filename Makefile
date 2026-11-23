# [Makefile] make: 서버 + CGI 빌드 / make test: 단위 테스트 / make asan: 메모리 검사 빌드·테스트 / make clean
CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -g -O2
CFLAGS  += -pthread -Isrc -MMD -MP $(SAN)
LDFLAGS += -pthread $(SAN)

# 일반 빌드는 build/, ASan 빌드는 build/asan/ 에 따로 둔다
# (같은 폴더를 쓰면 make asan 뒤 make 할 때 ASan용 .o가 섞여 링크가 실패함)
BUILD  ?= build
SERVER ?= bin/server

SRC   := $(filter-out src/main.c,$(wildcard src/*.c))
OBJ   := $(SRC:src/%.c=$(BUILD)/%.o)
TESTS := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

all: $(SERVER) cgi-bin/adder

$(SERVER): $(BUILD)/main.o $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

cgi-bin/adder: cgi-bin/adder.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BUILD)/test_%: tests/test_%.c $(OBJ)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# AddressSanitizer + UBSan: 메모리 누수·버퍼 오버플로우·정의되지 않은 동작 검사 → build/asan/server, build/asan/test_*
asan:
	$(MAKE) BUILD=build/asan SERVER=build/asan/server \
	        SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" build/asan/server test

clean:
	rm -rf bin build cgi-bin/adder cgi-bin/*.d

-include $(wildcard $(BUILD)/*.d)

.PHONY: all test asan clean
