EXEC = refs
SRC = refs.c
OBJS := \
	bitmap.o \
	refs.o \
	# intentionally left blank so last line can end in a \

REFS_DISK = refs_disk

CC	=	gcc
CFLAGS	=	-g -Og -Wall $(PKGFLAGS)

PKGFLAGS	=	`pkg-config fuse --cflags --libs`

# gcc -Wall -o fusexmp fusexmp.c `pkg-config fuse --cflags --libs`

all: $(OBJS)
	$(CC) -o $(EXEC) $(OBJS) $(CFLAGS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

# bitmap.c has a main method to test (commented out now)
# This rule compiles an executable called "bitmap"
bitmap: bitmap.c refs.h
	$(CC) -D TEST_BITMAP=1 -o bitmap bitmap.c $(CFLAGS)
#	$(CC) -o bitmap bitmap.o $(CFLAGS)

.PHONY: all clean

clean:
	rm -f $(EXEC) $(OBJS) $(REFS_DISK)

distclean:
	rm -f $(EXEC) $(OBJS) $(REFS_DISK) *~
