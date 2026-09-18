CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -pthread
LDLIBS  := -lglpk -lcurl -lm -pthread

SRC  := src/main.c src/http.c src/optimizer.c src/guardrails.c src/llm.c vendor/cJSON.c
TEST := tests/test_samples.c src/optimizer.c vendor/cJSON.c

all: gridwise

gridwise: $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

test: $(TEST)
	$(CC) $(CFLAGS) -o run_guardrails tests/test_guardrails.c src/guardrails.c src/optimizer.c vendor/cJSON.c -lglpk -lm
	./run_guardrails
	$(CC) $(CFLAGS) -o run_tests $(TEST) -lglpk -lm
	./run_tests

clean:
	rm -f gridwise run_tests run_guardrails

.PHONY: all test clean
