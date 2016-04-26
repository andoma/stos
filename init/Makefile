O ?= ${CURDIR}/obj
ARCH ?= x86_64
KHEADERS ?= /home/andoma/mixos/musl/kernel-headers
MODE ?= stos

SRCS =  src/main.c \
	src/util.c \
	src/logging.c \
	src/${MODE}.c \

SRCS += fsck/boot.c \
	fsck/charconv.c \
	fsck/check.c \
	fsck/common.c \
	fsck/fat.c \
	fsck/file.c \
	fsck/fsck.c \
	fsck/io.c \
	fsck/lfn.c


OBJS=    $(SRCS:%.c=$(O)/%.o)
DEPS=    ${OBJS:%.o=%.d}

CFLAGS += -std=c99 -O2 -Wall -Werror -I${KHEADERS}/${ARCH}/include

CC=${O}/musl/bin/musl-gcc

${O}/initrd: ${O}/stage/init Makefile
	(cd ${O}/stage && (find . -type f | cpio -H newc -o > $@))


${O}/musl-src/Makefile:
	@mkdir -p $(dir $@)
	curl -L http://www.musl-libc.org/releases/musl-1.1.14.tar.gz | tar xfz - --strip-components=1 -C "$(dir $@)"


${CC}: ${O}/musl-src/Makefile
	@mkdir -p $(dir $@)
	(cd "${O}/musl-src" && CC=${CROSS_COMPILE}gcc ./configure "--prefix=${O}/musl" --disable-shared && make -j4 install)

${O}/stage/init: ${OBJS} Makefile ${CC}
	@mkdir -p $(dir $@)
	${CC} -o $@ ${OBJS}

${O}/%.o: %.c Makefile ${CC}
	@mkdir -p $(dir $@)
	${CC} -MD -MP ${CFLAGS} -D_GNU_SOURCE -o $@ -c $<

install: ${O}/stage/init
	cp $< "${INITRD}/"

-include $(DEPS)
