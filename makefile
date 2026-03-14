CFLAGS=-Wall -Wextra -Werror -ggdb -O0

mcc: nob.o mcc.o parser.o lexer.o 
	cc -o mcc nob.o mcc.o parser.o lexer.o $(CFLAGS)

mcc.o: mcc.c parser.h
	cc -c mcc.c $(CFLAGS)

parser.o: parser.c parser.h
	cc -c parser.c $(CFLAGS)

lexer.o: lexer.h
	cc -c -x c -DMCC_LEXER_IMPLEMENTATION lexer.h

nob.o: nob.h
	cc -c -x c -DNOB_IMPLEMENTATION nob.h
