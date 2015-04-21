#
# Makefile for fspopulate
#

CLEANFILES	 = fspopulate
CFLAGS		+= -O2 -Werror -Wall -Wextra -m64 -fno-omit-frame-pointer
LDFLAGS		+= -lgen

fspopulate: fspopulate.c
	$(CC) -o $@ $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^

clean:
	rm -f $(CLEANFILES)
