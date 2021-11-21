/*
 * Ring buffers backed by littlefs files
 *
 * Copyright (c) 2021, Tobias Nießen. All rights reserved.
 * SPDX-License-Identifier: MIT
 */
#ifndef LFS_RINGBUFFER_H
#define LFS_RINGBUFFER_H

#include <lfs.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef LFSRING_YES_TRACE
#define LFSRING_TRACE(...) LFS_TRACE(__VA_ARGS__)
#else
#define LFSRING_TRACE(...)
#endif

/**
 * The default attribute that is used for ring buffer (also knows as "circular
 * buffer") metadata is 0xCB.
 */
#define LFSRING_DEFAULT_ATTR ((uint8_t) 0xCB)

/**
 * Mode of operation for a ring buffer.
 */
enum lfsring_mode {
  /**
   * In "stream" mode, a ring buffer is simply a sequence of bytes.
   *
   * Unlike in LFSRING_MODE_OBJECT, there is no grouping. In other words, the
   * partitioning of data when reading is arbitrary and unrelated to how the
   * data was partitioned across write operations.
   */
  LFSRING_MODE_STREAM,
  /**
   * In "object" mode, a ring buffer is a sequence of objects, which can only be
   * written and read as a whole.
   *
   * In this mode, the implementation ensures that the read position always
   * points to the beginning of an object, and no partial objects can be read
   * or written. When existing data has to be overwritten, the implementation
   * overwrites objects as a whole.
   */
  LFSRING_MODE_OBJECT
};

/**
 * Determines if existing data should be overwritten.
 */
enum lfsring_write_mode {
  /**
   * In this mode, fail instead of overwriting existing data.
   */
  LFSRING_NO_OVERWRITE,
  /**
   * In this mode, overwrite existing data, even if it has not been read yet.
   */
  LFSRING_OVERWRITE
};

typedef struct {
  void* file_buffer;
  uint8_t attr_metadata;
  lfs_size_t file_size;
  enum lfsring_mode mode;
} lfsring_config_t;

/**
 * A ring buffer backed by a littlefs file.
 *
 * Note that, even if the underlying block device is thread-safe, the high-level
 * ring buffer operations are not.
 */
typedef struct {
  lfs_t* backend;
  union {
    uint8_t bytes[12];
    struct {
      lfs_off_t read_low;
      lfs_off_t read_high;
      lfs_off_t write_dist;
    } le;
  } attr_buf;
  struct lfs_attr attr;
  struct lfs_file_config file_config;
  lfs_file_t file;
  lfs_size_t file_size;
  enum lfsring_mode mode;
} lfsring_t;

/**
 * Opens a ring buffer backed by a littlefs file.
 *
 * Note that, even if the underlying block device is thread-safe, the high-level
 * ring buffer operations are not.
 */
int lfsring_open(lfsring_t* ring, lfs_t* lfs, const char* path,
                 const lfsring_config_t* config);

/**
 * Checks if a ring buffer is empty.
 *
 * A ring buffer is empty if and only if the read position is equal to the write
 * position, that is, no data has been written that has not been removed yet.
 *
 * @param ring the ring buffer
 * @return true if the buffer is empty, false otherwise
 */
bool lfsring_is_empty(lfsring_t* ring);

/**
 * Appends data to a ring buffer.
 *
 * If the write mode is LFSRING_OVERWRITE, existing data will be discarded as
 * necessary to make room for new data. Otherwise, if there is not enough space
 * available to store the data, LFS_ERR_NOSPC will be returned and the buffer
 * remains unmodified.
 *
 * In LFSRING_MODE_STREAM, the buffer is allowed to be larger than the size of
 * the ring buffer itself if the write mode is LFSRING_OVERWRITE.
 *
 * In LFSRING_MODE_OBJECT, each successful call to this function creates a new
 * object within the ring buffer. Objects can only be retrieved as a whole,
 * which means that the buffer used to read objects must be large enough to hold
 * any object that is being written to the file.
 *
 * @param ring the ring buffer
 * @param data the data to write
 * @param data_size the number of bytes of data
 * @param write_mode whether to overwrite existing data
 */
int lfsring_append(lfsring_t* ring, const void* data, lfs_size_t data_size,
                   enum lfsring_write_mode write_mode);

/**
 * Reads data from a ring buffer without removing it.
 *
 * If the ring buffer was created as LFSRING_MODE_OBJECT, the supplied buffer
 * must be large enough to receive the next object. Otherwise, the buffer may
 * have any size.
 *
 * In LFSRING_MODE_STREAM, upon success, exactly buffer_size bytes are
 * retrieved, unless fewer bytes are available. If no data is available, the
 * function will return 0.
 *
 * In LFSRING_MODE_OBJECT, upon success, only one object is retrieved, and the
 * size of the object is returned. Objects may have a size of zero. If no object
 * is available in the buffer, LFS_ERR_NOENT is returned.
 *
 * @param ring the ring buffer
 * @param buffer where to write data to
 * @param buffer_size the maximum number of bytes to retrieve
 * @return number of bytes that have been retrieved, or a negative error code
 */
lfs_ssize_t lfsring_peek(lfsring_t* ring, void* buffer, lfs_size_t buffer_size);

/**
 * Reads data from a ring buffer and removes it.
 *
 * If the ring buffer was created as LFSRING_MODE_OBJECT, the supplied buffer
 * must be large enough to receive the next object. Otherwise, the buffer may
 * have any size.
 *
 * In LFSRING_MODE_STREAM, upon success, exactly buffer_size bytes are
 * retrieved, unless fewer bytes are available. If no data is available, the
 * function will return 0.
 *
 * In LFSRING_MODE_OBJECT, upon success, only one object is retrieved, and the
 * size of the object is returned. Objects may have a size of zero. If no object
 * is available in the buffer, LFS_ERR_NOENT is returned.
 *
 * @param ring the ring buffer
 * @param buffer where to write data to
 * @param buffer_size the maximum number of bytes to retrieve
 * @return number of bytes that have been retrieved, or a negative error code
 */
lfs_ssize_t lfsring_take(lfsring_t* ring, void* buffer, lfs_size_t buffer_size);

/**
 * Moves the read position forward, effectively discarding data.
 *
 * In LFSRING_MODE_STREAM, the distance is the number of bytes.
 *
 * In LFSRING_MODE_OBJECT, the distance is the number of objects.
 */
int lfsring_drop(lfsring_t* ring, lfs_off_t n);

/**
 * Closes a ring buffer.
 *
 * @param ring the ring buffer
 */
int lfsring_close(lfsring_t* ring);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LFS_RINGBUFFER_H
