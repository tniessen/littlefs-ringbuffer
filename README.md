# littlefs ring buffers

[Circular buffers][] backed by [littlefs][] files.

This library consists of a single header file and a single source file that
implement a simple circular data structure. Similar to littlefs itself, the
implementation focuses on memory efficiency (adding only a few bytes on top of
the required littlefs data structures) and reliability.

## Modes of operation

Ring buffers support two modes of operation. In "stream" mode, all data is
a contiguous sequence of bytes. In "object" mode, the implementation dynamically
partitions the buffer to store separate objects, which are variable-length
sequences of bytes themselves.

[Circular buffers]: https://en.wikipedia.org/wiki/Circular_buffer
[littlefs]: https://github.com/littlefs-project/littlefs
