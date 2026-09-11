<$MKROOT/$objtype/mkfile

TARG=eezoc

OFILES=\
	main.$O\
	ast.$O\
	bracket.$O\
	compile.$O\
	lib/lexeme.$O\
	lib/node.$O\
	parse/lexeme.$O\
	parse/tree.$O\
	parse/stack.$O\
	parse/array.$O\
	parse/pool.$O\
	parse/freelist.$O\
	parse/readfile.$O\
	parse/util.$O\
	parse/lex/lex.$O\
	parse/opp/operator.$O\
	parse/opp/shift.$O\
	parse/tokens.$O\
	parse/patterns.$O\
	parse/brackets.$O\
	parse/define.$O\
	parse/syntax.$O\
	parse/bind.$O\
	parse/parse.$O\

HFILES=\
	../libeezo/native.h\
	../libeezo/term.h\
	lib/lexeme.h\
	lib/node.h\
	ast.h\
	bracket.h\
	compile.h\
	parse/tree.h\
	parse/array.h\
	parse/stack.h\
	parse/pool.h\
	parse/freelist.h\
	parse/lexeme.h\
	parse/util.h\
	parse/readfile.h\
	parse/term.h\
	parse/parse.h\
	parse/syntax.h\
	parse/lex/lex.h\
	parse/opp/operator.h\

CFLAGS=-g -O2 -Wall -I.

LIBS=../libeezo

<$MKROOT/proto/mkone