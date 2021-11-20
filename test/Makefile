LFS_VERSION ?= 2.4.1
LIB_SOURCE = ../src/lfs_ringbuffer.c
TEST_SOURCE = test_ringbuffer.c
LFS_SOURCES_RELATIVE = lfs.c lfs_util.c bd/lfs_rambd.c
LFS_SOURCES = $(addprefix littlefs-$(LFS_VERSION)/,$(LFS_SOURCES_RELATIVE))
INCLUDE_DIRS = ../include littlefs-$(LFS_VERSION) littlefs-$(LFS_VERSION)/bd
C_FLAGS = -std=c99 -O3 -Wall -Wextra -Werror -pedantic

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
	$(CC) $(C_FLAGS) $(addprefix -I,$(INCLUDE_DIRS)) -o $@ $^
