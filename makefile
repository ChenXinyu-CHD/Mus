CFLAGS=-Wall -Wextra -Werror -ggdb -O0

mcc: main.c
	cc -o mcc main.c $(CFLAGS)
