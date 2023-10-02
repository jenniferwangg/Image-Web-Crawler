CC = gcc 
CFLAGS_XML2 = $(shell xml2-config --cflags)
CFLAGS_CURL = $(shell curl-config --cflags)
CFLAGS = -Wall $(CFLAGS_XML2) $(CFLAGS_CURL) -std=gnu99 -g -DDEBUG1 -Ilib
LD = gcc
LDFLAGS = -std=gnu99 -g 
LDLIBS_XML2 = $(shell xml2-config --libs)
LDLIBS_CURL = $(shell curl-config --libs)
LDLIBS = $(LDLIBS_XML2) $(LDLIBS_CURL) -pthread -lz

OBJ_DIR = obj
LIB_UTIL = $(OBJ_DIR)/htab.o
SRC_DIR = lib
SRCS   = findpng2.c htab.c
OBJS3  = $(OBJ_DIR)/findpng2.o $(LIB_UTIL)

TARGETS= findpng2

all: ${TARGETS}

findpng2: $(OBJS3) 
	mkdir -p $(OBJ_DIR)
	$(LD) -o $@ $^ $(LDLIBS) $(LDFLAGS) 

$(OBJ_DIR)/findpng2.o: findpng2.c
	mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c 
	mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@


%.o: %.c 
	$(CC) $(CFLAGS) -c $< 

%.d: %.c
	gcc -MM -MF $@ $<

-include $(SRCS:.c=.d)

.PHONY: clean
clean:
	rm -f *~ *.d *.o $(TARGETS) *.png *.html
