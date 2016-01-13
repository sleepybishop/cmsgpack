SOURCES=\
msgpack.c\
cJSON.c

CFLAGS  = -O2 -std=c99 -Wpointer-arith -D_DEFAULT_SOURCE -Wall 
LDFLAGS += -lm

OBJECTS = $(SOURCES:%.c=%.o)

all: mp2json json2mp

mp2json: mp2json.c $(OBJECTS)
	$(CC) $(CFLAGS) -o$@ $< $(OBJECTS) $(LDFLAGS)

json2mp: json2mp.c $(OBJECTS)
	$(CC) $(CFLAGS) -o$@ $< $(OBJECTS) $(LDFLAGS)

.PHONY: clean
clean:
	$(RM) mp2json json2mp *.o

.PHONY: indent
indent:
	clang-format -style=LLVM -i *.c *.h
