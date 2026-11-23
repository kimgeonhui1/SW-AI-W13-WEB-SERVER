# [Makefile] make: 서버 + CGI 빌드 / make test: 단위 테스트 / make asan: 메모리 검사 빌드 / make clean
CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -g -O2
CFLAGS  += -pthread -Isrc -MMD -MP
LDFLAGS += -pthread

SRC   := $(filter-out src/main.c,$(wildcard src/*.c))
OBJ   := $(SRC:src/%.c=build/%.o)
TESTS := $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

all: bin/server cgi-bin/adder

bin/server: build/main.o $(OBJ) | bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

cgi-bin/adder: cgi-bin/adder.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

build/test_%: tests/test_%.c $(OBJ) | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# AddressSanitizer + UBSan: 메모리 누수·버퍼 오버플로우·정의되지 않은 동작 검사
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean all test

bin build:
	mkdir -p $@

clean:
	rm -rf bin build cgi-bin/adder

-include $(wildcard build/*.d)

.PHONY: all test asan clean
