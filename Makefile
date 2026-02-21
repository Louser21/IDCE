all: idce

idce: lex.yy.c parser.tab.c main.cpp ir.h
	g++ -g -o idce main.cpp parser.tab.c lex.yy.c -I.

lex.yy.c: lexer.l parser.tab.h
	flex lexer.l

parser.tab.c parser.tab.h: parser.y
	bison -d -v parser.y

clean:
	rm -f lex.yy.c parser.tab.c parser.tab.h parser.output idce

run: idce
	./idce < input.ssa

debug: idce
	gdb ./idce