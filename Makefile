CFLAGS+=-Wall -g

all: slowsmtp deliver

slowsmtp: main.o
	gcc $^ -o $@

deliver: deliver.c
	gcc $^ $(CFLAGS) -lssl -o $@

