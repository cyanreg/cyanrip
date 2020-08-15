/* Create */
AVBufferRef *RENAME(fifo_create)(int max_queued, FNAME block_flags); /* -1 = INF, 0 = none */
AVBufferRef *RENAME(fifo_ref)(AVBufferRef *src, int max_queued, FNAME block_flags);

/* Query */
int RENAME(fifo_is_full)(AVBufferRef *src);
int RENAME(fifo_get_size)(AVBufferRef *src);
int RENAME(fifo_get_max_size)(AVBufferRef *src);

/* Modify */
void RENAME(fifo_set_max_queued)(AVBufferRef *dst, int max_queued);
void RENAME(fifo_set_block_flags)(AVBufferRef *dst, FNAME block_flags);

/* Up/downstreaming */
int RENAME(fifo_mirror)(AVBufferRef *dst, AVBufferRef *src);
int RENAME(fifo_unmirror)(AVBufferRef *dst, AVBufferRef *src);
int RENAME(fifo_unmirror_all)(AVBufferRef *dst);

/* I/O */
int   RENAME(fifo_push)(AVBufferRef *dst, TYPE *in);
TYPE *RENAME(fifo_pop)(AVBufferRef *src);
TYPE *RENAME(fifo_peek)(AVBufferRef *src);
