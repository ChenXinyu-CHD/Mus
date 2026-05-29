CC?=clang
CFLAGS=-Wall -Wextra -ggdb -O0

mcc: mcc.o parser.o lexer.o
	$(CC) -o mcc mcc.o parser.o lexer.o $(CFLAGS)

mcc.o: mcc.c parser.h lexer.h
	$(CC) -c mcc.c $(CFLAGS)

parser.o: parser.c parser.h lexer.h ast.h SymbolTable.h
	$(CC) -c parser.c $(CFLAGS)

lexer.o: lexer.h
	$(CC) -c -x c -DMCC_LEXER_IMPLEMENTATION $(CFLAGS) lexer.h
