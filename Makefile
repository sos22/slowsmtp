CFLAGS+=-Wall -g

all: slowsmtp

slowsmtp: main.o
	gcc $^ -o $@

