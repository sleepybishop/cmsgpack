OBJECTS=\
msgpack.o\
cJSON.o

CPPFLAGS = -D_DEFAULT_SOURCE
CFLAGS   = -O2 -std=c99 -Wall -Wpointer-arith
LDFLAGS  = -lm libcmsgpack.a

all: mp2json json2mp libcmsgpack.a

libcmsgpack.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)

mp2json: mp2json.o libcmsgpack.a

json2mp: json2mp.o libcmsgpack.a 

.PHONY: clean indent scan
clean:
	$(RM) mp2json json2mp *.o *.a

indent:
	clang-format -style=LLVM -i *.c *.h

scan:
	scan-build $(MAKE) clean all

