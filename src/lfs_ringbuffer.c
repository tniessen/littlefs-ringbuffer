#include "lfs_ringbuffer.h"

#include <string.h>

int lfsring_open(lfsring_t* ring, lfs_t* lfs, const char* path,
                 const lfsring_config_t* config) {
  LFSRING_TRACE("lfsring_open(%p, %p, \"%s\", %p {  })", (void*) ring, (void*) lfs, path, (void*) config);

  ring->attr.type = config->attr_metadata;
  ring->attr.buffer = ring->attr_buf.bytes;
  ring->attr.size = sizeof(ring->attr_buf.bytes);

  LFS_ASSERT(sizeof(ring->attr_buf.bytes) == sizeof(ring->attr_buf));

  // If the attribute does not exist, littlefs will silently create it. Thus,
  // we need to initialize the buffer.
  memset(ring->attr_buf.bytes, 0, sizeof(ring->attr_buf.bytes));

  memset(&ring->file_config, 0, sizeof(ring->file_config));
  ring->file_config.buffer = config->file_buffer;
  ring->file_config.attrs = &ring->attr;
  ring->file_config.attr_count = 1;

  int flags = LFS_O_CREAT | LFS_O_RDWR;
  int lfs_err = lfs_file_opencfg(lfs, &ring->file, path, flags, &ring->file_config);
  if (lfs_err != 0) {
    return lfs_err;
  }

  ring->backend = lfs;
  ring->mode = config->mode;
  ring->file_size = config->file_size;

  return 0;
}

static inline uint64_t get_pos_r(lfsring_t* ring) {
  uint64_t low = lfs_fromle32(ring->attr_buf.le.read_low);
  uint64_t high = lfs_fromle32(ring->attr_buf.le.read_high);
  return (high << 32) | low;
}

static inline uint64_t get_pos_w(lfsring_t* ring) {
  return get_pos_r(ring) + lfs_fromle32(ring->attr_buf.le.write_dist);
}

static int do_write(lfsring_t* ring, const void* data, lfs_size_t sz, lfs_off_t rel_off) {
  LFS_ASSERT(sz < ring->file_size);

  lfs_off_t write_offset = (get_pos_w(ring) + rel_off) % ring->file_size;
  lfs_soff_t seeked = lfs_file_seek(ring->backend, &ring->file, write_offset, LFS_SEEK_SET);
  if (seeked < 0) {
    return seeked;
  }
  LFS_ASSERT(write_offset == (lfs_off_t) seeked);

  lfs_size_t avail = ring->file_size - write_offset;
  lfs_size_t fit = lfs_min(avail, sz);

  lfs_ssize_t written = lfs_file_write(ring->backend, &ring->file, data, fit);
  if (written < 0) {
    return written;
  }
  LFS_ASSERT(fit == (lfs_size_t) written);

  if (fit < sz) {
    int err = lfs_file_rewind(ring->backend, &ring->file);
    if (err) {
      return err;
    }

    written = lfs_file_write(ring->backend, &ring->file, ((const uint8_t*) data) + fit, sz - fit);
    if (written < 0) {
      return written;
    }
    LFS_ASSERT(sz - fit == (lfs_size_t) written);
  }

  return lfs_file_sync(ring->backend, &ring->file);
}

static int do_read(lfsring_t* ring, void* data, lfs_size_t sz, lfs_off_t rel_off) {
  LFS_ASSERT(sz < ring->file_size);

  lfs_off_t read_offset = (get_pos_r(ring) + rel_off) % ring->file_size;
  lfs_soff_t seeked = lfs_file_seek(ring->backend, &ring->file, read_offset, LFS_SEEK_SET);
  if (seeked < 0) {
    return seeked;
  }
  LFS_ASSERT(read_offset == (lfs_off_t) seeked);

  lfs_size_t avail = ring->file_size - read_offset;
  lfs_size_t fit = lfs_min(avail, sz);

  lfs_ssize_t n_read = lfs_file_read(ring->backend, &ring->file, data, fit);
  if (n_read < 0) {
    return n_read;
  }
  if ((lfs_size_t) n_read < fit) {
    // littlefs will only read fewer bytes than requested if we reached the end
    // of the file, however, at this point, we are certain that the byte range
    // should exist.
    return LFS_ERR_CORRUPT;
  }
  LFS_ASSERT(fit == (lfs_size_t) n_read);

  if (fit < sz) {
    int err = lfs_file_rewind(ring->backend, &ring->file);
    if (err) {
      return err;
    }

    n_read = lfs_file_read(ring->backend, &ring->file, ((uint8_t*) data) + fit, sz - fit);
    if (n_read < 0) {
      return n_read;
    }
    if ((lfs_size_t) n_read < sz - fit) {
      // littlefs will only read fewer bytes than requested if we reached the
      // end of the file, however, at this point, we are certain that the byte
      // range should exist.
      return LFS_ERR_CORRUPT;
    }
    LFS_ASSERT(sz - fit == (lfs_size_t) n_read);
  }

  return 0;
}

static int advance_write_position(lfsring_t* ring, lfs_size_t distance) {
  lfs_off_t old_write_dist = lfs_fromle32(ring->attr_buf.le.write_dist);
  LFS_ASSERT(old_write_dist + distance <= ring->file_size);
  ring->attr_buf.le.write_dist = lfs_tole32(old_write_dist + distance);

  // This is a hack. littlefs won't update attributes unless the file was
  // modified, too.
  // TODO: find a workaround that does not meddle with lfs internals
  ring->file.flags |= LFS_F_DIRTY;

  int err = lfs_file_sync(ring->backend, &ring->file);
  if (err) {
    // TODO: undo changes?
    return err;
  }

  return 0;
}

static int advance_read_position(lfsring_t* ring, lfs_size_t distance) {
  lfs_off_t old_write_dist = lfs_fromle32(ring->attr_buf.le.write_dist);
  LFS_ASSERT(distance <= old_write_dist);

  uint64_t new_read_pos = get_pos_r(ring) + distance;
  ring->attr_buf.le.read_high = lfs_tole32(new_read_pos >> 32);
  ring->attr_buf.le.read_low = lfs_tole32(new_read_pos & UINT32_MAX);
  ring->attr_buf.le.write_dist = lfs_tole32(old_write_dist - distance);

  // This is a hack. littlefs won't update attributes unless the file was
  // modified, too.
  // TODO: find a workaround that does not meddle with lfs internals
  ring->file.flags |= LFS_F_DIRTY;

  int err = lfs_file_sync(ring->backend, &ring->file);
  if (err) {
    // TODO: undo changes?
    return err;
  }

  return 0;
}

bool lfsring_is_empty(lfsring_t* ring) {
  return ring->attr_buf.le.write_dist == 0;
}

int lfsring_append(lfsring_t* ring, const void* data, lfs_size_t data_size,
                   enum lfsring_write_mode write_mode) {
  LFSRING_TRACE("lfsring_append(%p, %p, %u, %d)", (void*) ring, data, data_size, write_mode);

  if (write_mode != LFSRING_NO_OVERWRITE && write_mode != LFSRING_OVERWRITE) {
    return LFS_ERR_INVAL;
  }

  lfs_size_t available_size = ring->file_size - lfs_fromle32(ring->attr_buf.le.write_dist);

  if (ring->mode == LFSRING_MODE_OBJECT) {
    lfs_size_t eff_avail = available_size;
    if (write_mode == LFSRING_OVERWRITE) {
      eff_avail = ring->file_size;
    }
    if (eff_avail < sizeof(lfs_size_t)) {
      return LFS_ERR_NOSPC;
    }
    lfs_size_t max_obj_size = eff_avail - sizeof(lfs_size_t);
    if (data_size > max_obj_size) {
      return LFS_ERR_NOSPC;
    }
  } else if (write_mode == LFSRING_OVERWRITE) {
    if (data_size > ring->file_size) {
      // If the buffer is too small to hold all of the data, only store the
      // last part.
      // TODO: consider moving both the read and the write position accordingly
      data = ((const uint8_t*) data) + (data_size - ring->file_size);
      data_size = ring->file_size;
    }
  } else if (data_size > available_size) {
    return LFS_ERR_NOSPC;
  }

  lfs_size_t write_size = data_size;
  if (ring->mode == LFSRING_MODE_OBJECT) {
    write_size += sizeof(lfs_size_t);
  }

  // If we are going to overwrite existing data (i.e., data that would be
  // returned by a subsequent read request), we pre-emptively move the read
  // position forward.
  if (write_mode == LFSRING_OVERWRITE) {
    LFSRING_TRACE("write_size=%u available_size=%u", write_size, available_size);
    if (write_size > available_size) {
      lfs_size_t overlap_size = write_size - available_size;
      if (ring->mode == LFSRING_MODE_OBJECT) {
        lfs_size_t skippable = lfs_fromle32(ring->attr_buf.le.write_dist);
        lfs_off_t dropped = 0;
        while (dropped < overlap_size) {
          LFS_ASSERT(dropped < skippable);

          if (skippable - dropped < sizeof(lfs_size_t)) {
            return LFS_ERR_CORRUPT;
          }

          lfs_size_t obj_size;
          int err = do_read(ring, &obj_size, sizeof(obj_size), dropped);
          if (err) {
            return err;
          }
          obj_size = lfs_fromle32(obj_size);
          dropped += sizeof(lfs_size_t);

          LFS_ASSERT(dropped <= skippable);

          if (skippable - dropped < obj_size) {
            return LFS_ERR_CORRUPT;
          }

          dropped += obj_size;
        }
        overlap_size = dropped;
      }
      int err = advance_read_position(ring, overlap_size);
      if (err) {
        return err;
      }
    }
  }

  // In object mode, write the size of the object as a 32-bit integer before the
  // actual data (i.e., the object).
  if (ring->mode == LFSRING_MODE_OBJECT) {
    lfs_size_t obj_size = lfs_tole32(data_size);
    int err = do_write(ring, &obj_size, sizeof(lfs_size_t), 0);
    if (err) {
      return err;
    }
  }

  // We have ensured that there is enough space, so write the data.
  int err = do_write(ring, data, data_size, (ring->mode == LFSRING_MODE_OBJECT) ? sizeof(lfs_size_t) : 0);
  if (err) {
    return err;
  }

  err = advance_write_position(ring, write_size);
  if (err) {
    return err;
  }

  return 0;
}

lfs_ssize_t lfsring_peek(lfsring_t* ring, void* buffer, lfs_size_t buffer_size) {
  LFSRING_TRACE("lfsring_peek(%p, %p, %u)", (void*) ring, buffer, buffer_size);

  lfs_size_t avail = lfs_fromle32(ring->attr_buf.le.write_dist);

  if (ring->mode == LFSRING_MODE_OBJECT) {
    if (avail == 0) {
      // Unlike in stream mode, we cannot return 0 here because objects can be
      // empty (i.e., have a size of 0 bytes) and the caller must be able to
      // distinguish between "no object" and "empty object".
      return LFS_ERR_NOENT;
    } else if (avail < sizeof(lfs_size_t)) {
      // Objects always begin with four bytes that encode the size of the
      // object. If there are fewer bytes in the buffer, the file is corrupt.
      return LFS_ERR_CORRUPT;
    } else {
      // Read the size of the object first.
      lfs_size_t obj_size;
      int err = do_read(ring, &obj_size, sizeof(obj_size), 0);
      if (err) {
        return err;
      }
      obj_size = lfs_fromle32(obj_size);
      // If there are fewer bytes available than the size of the object, the
      // file is corrupt.
      if (avail - sizeof(lfs_size_t) < obj_size) {
        return LFS_ERR_CORRUPT;
      }
      // If the buffer provided by the user is too small to retrieve the entire
      // object, fail to ensure that objects are only retrieved as a whole.
      if (obj_size > buffer_size) {
        return LFS_ERR_NOMEM;
      }
      buffer_size = obj_size;
    }
  } else {
    // Do not read more bytes than available.
    buffer_size = lfs_min(avail, buffer_size);
  }

  int err = do_read(ring, buffer, buffer_size, (ring->mode == LFSRING_MODE_OBJECT) ? sizeof(lfs_size_t) : 0);
  if (err) {
    return err;
  }

  return buffer_size;
}

lfs_ssize_t lfsring_take(lfsring_t* ring, void* buffer, lfs_size_t buffer_size) {
  LFSRING_TRACE("lfsring_take(%p, %p, %u)", (void*) ring, buffer, buffer_size);

  lfs_ssize_t ret = lfsring_peek(ring, buffer, buffer_size);
  if (ret < 0) {
    return ret;
  }

  LFS_ASSERT((lfs_size_t) ret <= buffer_size);

  int err = advance_read_position(ring, (lfs_size_t) ret + ((ring->mode == LFSRING_MODE_OBJECT) ? sizeof(lfs_size_t) : 0));
  if (err) {
    return err;
  }

  return ret;
}

int lfsring_drop(lfsring_t* ring, lfs_off_t n) {
  LFSRING_TRACE("lfsring_drop(%p, %u)", (void*) ring, n);

  lfs_size_t avail = lfs_fromle32(ring->attr_buf.le.write_dist);

  if (ring->mode == LFSRING_MODE_STREAM) {
    if (n > avail) {
      return LFS_ERR_INVAL;
    }

    return advance_read_position(ring, n);
  } else {
    lfs_off_t dropped = 0;
    while (n-- > 0) {
      if (avail == dropped) {
        return LFS_ERR_INVAL;
      }

      LFS_ASSERT(dropped < avail);

      if (avail - dropped < sizeof(lfs_size_t)) {
        return LFS_ERR_CORRUPT;
      }

      lfs_size_t obj_size;
      int err = do_read(ring, &obj_size, sizeof(obj_size), dropped);
      if (err) {
        return err;
      }
      obj_size = lfs_fromle32(obj_size);
      dropped += sizeof(lfs_size_t);

      LFS_ASSERT(dropped <= avail);

      if (avail - dropped < obj_size) {
        return LFS_ERR_CORRUPT;
      }

      dropped += obj_size;
    }

    return advance_read_position(ring, dropped);
  }
}

int lfsring_close(lfsring_t* ring) {
  LFSRING_TRACE("lfsring_close(%p)", (void*) ring);
  return lfs_file_close(ring->backend, &ring->file);
}
