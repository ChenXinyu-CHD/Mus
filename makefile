CFLAGS=-Wall -Wextra -ggdb -O0 -Wno-unused-function

mcc: mcc.o parser.o lexer.o
	cc -o mcc mcc.o parser.o lexer.o $(CFLAGS)

mcc.o: mcc.c parser.h lexer.h
	cc -c mcc.c $(CFLAGS)

parser.o: parser.c parser.h lexer.h
	cc -c parser.c $(CFLAGS)

lexer.o: lexer.h
	cc -c -x c -DMCC_LEXER_IMPLEMENTATION $(CFLAGS) lexer.h
