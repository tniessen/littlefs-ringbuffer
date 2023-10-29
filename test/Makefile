LFS_VERSION ?= 2.6.1
LIB_SOURCE = ../src/lfs_ringbuffer.c
TEST_SOURCE = test_ringbuffer.c
LFS_SOURCES_RELATIVE = lfs.c lfs_util.c bd/lfs_rambd.c
LFS_SOURCES = $(addprefix littlefs-$(LFS_VERSION)/,$(LFS_SOURCES_RELATIVE))
INCLUDE_DIRS = ../include littlefs-$(LFS_VERSION) littlefs-$(LFS_VERSION)/bd
CFLAGS = -std=c99 -O3 -Wall -Wextra -Werror -pedantic

# Some versions of gcc emit warnings with littlefs 2.4.x, which almost certainly
# are false positives.
CC_IS_GCC = $(shell $(CC) --version | head -1 | grep -c ^gcc)
LFS_TRIGGERS_GCC_WARNING = $(shell echo $(LFS_VERSION) | grep -c "^2\.4\.")
ifeq "$(CC_IS_GCC)" "1"
	ifeq "$(LFS_TRIGGERS_GCC_WARNING)" "1"
		CFLAGS += -Wno-array-bounds -Wno-uninitialized
	endif
endif

.PHONY: test
test: dependencies test_ringbuffer
	@echo Running tests
	./test_ringbuffer

.PHONY: dependencies
dependencies: littlefs-$(LFS_VERSION)

littlefs-$(LFS_VERSION):
	@echo Downloading littlefs v$(LFS_VERSION)
	curl -sL "https://github.com/littlefs-project/littlefs/archive/refs/tags/v$(LFS_VERSION).tar.gz" | tar -xzf - -C .

test_ringbuffer: $(LIB_SOURCE) $(TEST_SOURCE) $(LFS_SOURCES)
	$(CC) $(CFLAGS) $(addprefix -I,$(INCLUDE_DIRS)) -o $@ $^

.PHONY: clean
clean:
	rm -f test_ringbuffer
