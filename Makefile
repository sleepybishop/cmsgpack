OBJECTS=\
msgpack.o

CPPFLAGS += -D_DEFAULT_SOURCE
CFLAGS += -O2 -std=c99 -Wall -Wpointer-arith

all: libcmsgpack.a

libcmsgpack.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)

.PHONY: clean indent scan
clean:
	$(RM) mp2json json2mp *.o *.a

indent:
	clang-format -style=LLVM -i *.c *.h

scan:
	scan-build $(MAKE) clean all

