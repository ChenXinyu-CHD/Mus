CFLAGS=-Wall -Wextra -Werror -ggdb -O0

mcc: nob.o mcc.o parser.o lexer.o utils.o
	cc -o mcc nob.o mcc.o parser.o lexer.o utils.o $(CFLAGS)

mcc.o: mcc.c parser.h
	cc -c mcc.c $(CFLAGS)

parser.o: parser.c parser.h
	cc -c parser.c $(CFLAGS)

lexer.o: lexer.h
	cc -c -x c -DMCC_LEXER_IMPLEMENTATION $(CFLAFS) lexer.h

nob.o: nob.h
	cc -c -x c -DNOB_IMPLEMENTATION $(CFLAFS) nob.h

utils.o: utils.h
	cc -c -x c -DMCC_UTILS_IMPLEMENTATION $(CFLAFS) utils.h
