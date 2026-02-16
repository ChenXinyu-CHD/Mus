CFLAGS=-Wall -Wextra -Werror -ggdb -O0

mcc: mcc.o parser.o
	cc -o mcc mcc.o parser.o $(CFLAGS)

mcc.o: mcc.c parser.h
	cc -c mcc.c $(CFLAGS)

parser.o: parser.c parser.h
	cc -c parser.c $(CFLAGS)
