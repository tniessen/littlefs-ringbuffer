#include <assert.h>

#include <lfs_ringbuffer.h>

#include <lfs_rambd.h>

#define LFS_READ_SIZE      16
#define LFS_PROG_SIZE      LFS_READ_SIZE
#define LFS_BLOCK_SIZE     512
#define LFS_BLOCK_COUNT    1024
#define LFS_BLOCK_CYCLES   (-1)
#define LFS_CACHE_SIZE     (64 % (LFS_PROG_SIZE) == 0 ? 64 : (LFS_PROG_SIZE))
#define LFS_LOOKAHEAD_SIZE 16

static void test_stream_mode(lfs_t* fs) {
  const char* path = "stream.cb";

  uint8_t buffer[200];

  lfsring_config_t config = {
    .attr_metadata = LFSRING_DEFAULT_ATTR,
    .mode = LFSRING_MODE_STREAM,
    .file_size = 4 * 1024
  };

  lfsring_t rbuf;
  int err = lfsring_open(&rbuf, fs, path, &config);
  assert(err == 0);

  // The ring buffer should be empty initially.
  assert(lfsring_is_empty(&rbuf));

  // Fill the file with a string.
  const char* msg = "Hello world";
  for (unsigned int i = 0; i < 4 * 1024 / strlen(msg); i++) {
    err = lfsring_append(&rbuf, msg, strlen(msg), LFSRING_NO_OVERWRITE);
    assert(err == 0);
    assert(!lfsring_is_empty(&rbuf));
  }

  for (unsigned int i = 0; i < 100; i++) {
    // Close the file occasionally to ensure that the buffer retains its state.
    if (i % 7 == 4) {
      err = lfsring_close(&rbuf);
      assert(err == 0);
      err = lfsring_open(&rbuf, fs, path, &config);
      assert(err == 0);
      assert(!lfsring_is_empty(&rbuf));
    }

    // There should not be enough room within the buffer for this operation.
    err = lfsring_append(&rbuf, msg, strlen(msg), LFSRING_NO_OVERWRITE);
    assert(err == LFS_ERR_NOSPC);

    // Make some room by either "taking" or "dropping" data.
    if (i % 3 == 0) {
      buffer[strlen(msg)] = 0;
      lfs_ssize_t ret = lfsring_take(&rbuf, buffer, strlen(msg));
      assert(ret == (lfs_ssize_t) strlen(msg));
      assert(strlen((const char*) buffer) == strlen(msg));
      assert(strcmp((const char*) buffer, msg) == 0);
    } else {
      err = lfsring_drop(&rbuf, strlen(msg));
      assert(err == 0);
    }

    // We only removed a small amount of data, the buffer should still be
    // almost full, and definitely not empty.
    assert(!lfsring_is_empty(&rbuf));

    // Close the file occasionally to ensure that the buffer retains its state.
    if (i % 5 == 1) {
      err = lfsring_close(&rbuf);
      assert(err == 0);
      err = lfsring_open(&rbuf, fs, path, &config);
      assert(err == 0);
    }

    // Write to the newly available space.
    err = lfsring_append(&rbuf, msg, strlen(msg), LFSRING_NO_OVERWRITE);
    assert(err == 0);
  }

  // Unlike in object mode, we can read any number of bytes at once, ignoring
  // how the data was written.
  assert(sizeof(buffer) > strlen(msg));
  for (lfs_size_t sz = 0; sz <= sizeof(buffer); sz++) {
    memset(buffer, 0, sizeof(buffer));
    lfs_ssize_t n_read = lfsring_peek(&rbuf, buffer, sz);
    assert(n_read >= 0 && (lfs_size_t) n_read == sz);
    for (unsigned int i = 0; i < sz; i++) {
      assert((char) buffer[i] == msg[i % strlen(msg)]);
    }
  }

  // At this point, the file should have reached its maximum size and should not
  // have grown beyond that.
  struct lfs_info file_info;
  err = lfs_stat(fs, path, &file_info);
  assert(err == 0);
  assert(file_info.size == config.file_size);

  // The file is not "full" since the size of the string does not divide the
  // size of the file. Attempting to drop "all" of the file should fail.
  assert(!lfsring_is_empty(&rbuf));
  assert(config.file_size % strlen(msg) != 0);
  err = lfsring_drop(&rbuf, config.file_size);
  assert(err == LFS_ERR_INVAL);

  // However, dropping all of the data at once should work.
  assert(!lfsring_is_empty(&rbuf));
  err = lfsring_drop(&rbuf, strlen(msg) * (4 * 1024 / strlen(msg)));
  assert(err == 0);
  assert(lfsring_is_empty(&rbuf));

  // Fill the buffer again, this time with LFSRING_OVERWRITE, which should have
  // no effect since the buffer is empty and has enough space.
  for (unsigned int i = 0; i < 4 * 1024 / strlen(msg); i++) {
    err = lfsring_append(&rbuf, msg, strlen(msg), LFSRING_OVERWRITE);
    assert(err == 0);
    assert(!lfsring_is_empty(&rbuf));
  }

  // At this point, writing with LFSRING_NO_OVERWRITE should fail.
  err = lfsring_append(&rbuf, msg, strlen(msg), LFSRING_NO_OVERWRITE);
  assert(err == LFS_ERR_NOSPC);

  // However, LFSRING_OVERWRITE should overwrite the first few bytes we added.
  err = lfsring_append(&rbuf, msg, strlen(msg), LFSRING_OVERWRITE);
  assert(err == 0);

  // This should have overwritten the first few bytes and moved the read
  // position to account for that.
  lfs_size_t n_bytes_overwritten = strlen(msg) - (config.file_size % strlen(msg));
  lfs_ssize_t ret = lfsring_take(&rbuf, buffer, strlen(msg) - n_bytes_overwritten);
  assert(ret >= 0 && (lfs_size_t) ret == strlen(msg) - n_bytes_overwritten);
  assert(strncmp((const char*) buffer, msg + n_bytes_overwritten, strlen(msg) - n_bytes_overwritten) == 0);

  // As few bytes as possible should have been overwritten, so all other strings
  // (including the one that overwrote the first string) should still be there.
  for (unsigned int i = 0; i < 4 * 1024 / strlen(msg); i++) {
    assert(!lfsring_is_empty(&rbuf));
    ret = lfsring_take(&rbuf, buffer, strlen(msg));
    assert(ret >= 0 && (lfs_size_t) ret == strlen(msg));
    assert(strncmp(msg, (const char*) buffer, strlen(msg)) == 0);
  }

  assert(lfsring_is_empty(&rbuf));

  err = lfsring_drop(&rbuf, 0);
  assert(err == 0);

  // We cannot remove what is not there.
  err = lfsring_drop(&rbuf, 1);
  assert(err == LFS_ERR_INVAL);

  err = lfsring_close(&rbuf);
  assert(err == 0);

  err = lfs_remove(fs, path);
  assert(err == 0);
}

static void test_object_mode(lfs_t* fs) {
  const char* path = "obj.cb";

  lfsring_config_t config = {
    .attr_metadata = LFSRING_DEFAULT_ATTR,
    .mode = LFSRING_MODE_OBJECT,
    .file_size = 4 * 1024
  };

  lfsring_t rbuf;
  int err = lfsring_open(&rbuf, fs, path, &config);
  assert(err == 0);

  assert(lfsring_is_empty(&rbuf));

  struct sample_obj {
    uint16_t foo;
    uint64_t bar;
    char msg[16];
  };

  const struct sample_obj a = {
    .foo = 12345,
    .bar = 0xaabbccddeeff,
    .msg = "Hello world"
  };

  // Write one "object" to the buffer.
  err = lfsring_append(&rbuf, &a, sizeof(a), LFSRING_NO_OVERWRITE);
  assert(err == 0);

  assert(!lfsring_is_empty(&rbuf));

  // Retrieve the object. This should be a flat copy of the original object.
  struct sample_obj b;
  memset(&b, 0, sizeof(struct sample_obj));
  lfs_ssize_t ret = lfsring_peek(&rbuf, &b, sizeof(struct sample_obj));
  assert(ret == sizeof(struct sample_obj));
  assert(memcmp(&a, &b, sizeof(struct sample_obj)) == 0);

  // peek should not remove the object.
  assert(!lfsring_is_empty(&rbuf));

  // Try retrieving the object with a buffer that is too small.
  memset(&b, 0, sizeof(struct sample_obj));
  ret = lfsring_peek(&rbuf, &b, sizeof(struct sample_obj) - 1);
  assert(ret == LFS_ERR_NOMEM);

  // Provide a buffer that is more than big enough (and use take instead of peek
  // this time).
  struct sample_obj multiple[5];
  ret = lfsring_take(&rbuf, multiple, sizeof(multiple));
  assert(ret == sizeof(struct sample_obj));
  assert(memcmp(&a, multiple, sizeof(struct sample_obj)) == 0);

  // We removed the only object.
  assert(lfsring_is_empty(&rbuf));

  // Unlike in stream mode, when no data is available, reading should yield an
  // error instead of 0.
  ret = lfsring_peek(&rbuf, multiple, sizeof(multiple));
  assert(ret == LFS_ERR_NOENT);
  ret = lfsring_take(&rbuf, multiple, sizeof(multiple));
  assert(ret == LFS_ERR_NOENT);

  // Append the original object five times.
  assert(lfsring_is_empty(&rbuf));
  for (unsigned int i = 0; i < 5; i++) {
    err = lfsring_append(&rbuf, &a, sizeof(a), LFSRING_NO_OVERWRITE);
    assert(err == 0);
    assert(!lfsring_is_empty(&rbuf));
  }

  // Provide a buffer that is large enough to receive all five objects. However,
  // the peek and take functions do not assume that the caller will be able to
  // tell where one object ends and where the next begins, so they should only
  // retrieve a single object.
  assert(sizeof(multiple) == 5 * sizeof(a));
  ret = lfsring_peek(&rbuf, multiple, sizeof(multiple));
  assert(ret == sizeof(struct sample_obj));
  assert(memcmp(&a, multiple, sizeof(struct sample_obj)) == 0);

  // Remove all five objects, one by one, alternating between take and drop.
  for (unsigned int i = 0; i < 5; i++) {
    assert(!lfsring_is_empty(&rbuf));

    if (i % 2 == 0) {
      ret = lfsring_take(&rbuf, multiple, sizeof(multiple));
      assert(ret == sizeof(struct sample_obj));
      assert(memcmp(&a, multiple, sizeof(struct sample_obj)) == 0);
    } else {
      err = lfsring_drop(&rbuf, 1);
      assert(err == 0);
    }
  }

  assert(lfsring_is_empty(&rbuf));

  // Fill the buffer with empty objects.
  for (unsigned int i = 0; i < config.file_size / sizeof(lfs_size_t); i++) {
    err = lfsring_append(&rbuf, NULL, 0, LFSRING_NO_OVERWRITE);
    assert(err == 0);
  }

  // The buffer should not have room for anything else, not even for another
  // zero-size object.
  err = lfsring_append(&rbuf, NULL, 0, LFSRING_NO_OVERWRITE);
  assert(err == LFS_ERR_NOSPC);

  // Dropping zero objects should succeed.
  err = lfsring_drop(&rbuf, 0);
  assert(err == 0);

  assert(!lfsring_is_empty(&rbuf));

  // Removing too many objects should fail.
  err = lfsring_drop(&rbuf, config.file_size / sizeof(lfs_size_t) + 1);
  assert(err == LFS_ERR_INVAL);

  assert(!lfsring_is_empty(&rbuf));

  // Removing all objects at once should work.
  err = lfsring_drop(&rbuf, config.file_size / sizeof(lfs_size_t));
  assert(err == 0);

  assert(lfsring_is_empty(&rbuf));

  // There are no objects left, dropping should fail now.
  err = lfsring_drop(&rbuf, 1);
  assert(err == LFS_ERR_INVAL);

  assert(lfsring_is_empty(&rbuf));

  // Dropping zero objects should always succeed.
  err = lfsring_drop(&rbuf, 0);
  assert(err == 0);

  // Add increasingly large objects with LFSRING_OVERWRITE.
  uint8_t large_buffer[0xff];
  for (unsigned int i = 0; i <= sizeof(large_buffer); i++) {
    memset(large_buffer, i, i);
    err = lfsring_append(&rbuf, large_buffer, i, LFSRING_OVERWRITE);
    assert(err == 0);
    assert(!lfsring_is_empty(&rbuf));

    // Determine which entries should stil be in the buffer (i.e., should not
    // have been overwritten).
    unsigned int j = i + 1, k = 0;
    do {
      k += sizeof(lfs_size_t) + --j;
    } while (j != 0 && k + sizeof(lfs_size_t) + (j - 1) <= config.file_size);

    // Check that the first entry in the buffer is the first that should not
    // have been overwritten.
    ret = lfsring_peek(&rbuf, large_buffer, sizeof(large_buffer));
    assert(ret >= 0 && (lfs_size_t) ret == j);
    for (unsigned int l = 0; l < j; l++) {
      assert(large_buffer[l] == j);
    }
  }

  // Drop all objects.
  while (!lfsring_is_empty(&rbuf)) {
    err = lfsring_drop(&rbuf, 1);
    assert(err == 0);
  }

  err = lfsring_close(&rbuf);
  assert(err == 0);

  err = lfs_remove(fs, path);
  assert(err == 0);
}

static void run_tests_with_config(struct lfs_config* fs_config) {
  lfs_t fs;
  int err = lfs_format(&fs, fs_config);
  assert(err == 0);

  err = lfs_mount(&fs, fs_config);
  assert(err == 0);

  test_stream_mode(&fs);
  test_object_mode(&fs);

  err = lfs_unmount(&fs);
  assert(err == 0);
}

int main(void) {
  lfs_rambd_t rambd;
  struct lfs_config fs_config = {
    .context        = &rambd,
    .read           = lfs_rambd_read,
    .prog           = lfs_rambd_prog,
    .erase          = lfs_rambd_erase,
    .sync           = lfs_rambd_sync,
    .read_size      = LFS_READ_SIZE,
    .prog_size      = LFS_PROG_SIZE,
    .block_size     = LFS_BLOCK_SIZE,
    .block_count    = LFS_BLOCK_COUNT,
    .block_cycles   = LFS_BLOCK_CYCLES,
    .cache_size     = LFS_CACHE_SIZE,
    .lookahead_size = LFS_LOOKAHEAD_SIZE
  };

  // We shouldn't need to set an erase value, but if we don't, the rambd won't
  // initialize the allocated memory at all, leading to memcheck warnings.
  struct lfs_rambd_config rambd_config = {
#if LFS_VERSION < 0x00020006
    .erase_value = 0,
#endif
    .buffer = NULL
  };

  // Allocate the in-memory block device.
  int err = lfs_rambd_createcfg(&fs_config, &rambd_config);
  assert(err == 0);

  run_tests_with_config(&fs_config);

  err = lfs_rambd_destroy(&fs_config);
  assert(err == 0);

  return 0;
}
