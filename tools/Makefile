CPPFLAGS+= -D_DEFAULT_SOURCE
CFLAGS+= -O2 -std=c99 -Wall -Wpointer-arith -I../ -I./cJSON
LDFLAGS+= -lm ../libcmsgpack.a

all: mp2json json2mp

cJSON:
	git clone https://github.com/DaveGamble/cJSON

cJSON/cJSON.h: cJSON
cJSON/cJSON.o: cJSON
	make -C cJSON

../libcmsgpack.a:
	make -C ../

mp2json.o: cJSON/cJSON.h
json2mp.o: cJSON/cJSON.h

mp2json: mp2json.o ../libcmsgpack.a cJSON/cJSON.o

json2mp: json2mp.o ../libcmsgpack.a cJSON/cJSON.o

.PHONY: clean indent scan
clean:
	$(RM) mp2json json2mp *.o *.a
	$(RM) -rf cJSON

indent:
	clang-format -style=LLVM -i *.c *.h

scan:
	scan-build $(MAKE) clean all

