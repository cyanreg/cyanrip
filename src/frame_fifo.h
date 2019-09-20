#include <pthread.h>
#include <assert.h>
#include <libavutil/frame.h>

typedef struct AVFrameFIFO {
    AVFrame **queued_frames;
    int num_queued_frames;
    int max_queued_frames;
    unsigned int queued_frames_alloc;
    pthread_mutex_t lock;
    pthread_cond_t cond_in;
    pthread_cond_t cond_out;
    pthread_mutex_t cond_lock_in;
    pthread_mutex_t cond_lock_out;
} AVFrameFIFO;

static inline int init_fifo(AVFrameFIFO *buf, int max_queued_frames)
{
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->cond_in, NULL);
    pthread_mutex_init(&buf->cond_lock_in, NULL);
    if (max_queued_frames > 0) {
        pthread_cond_init(&buf->cond_out, NULL);
        pthread_mutex_init(&buf->cond_lock_out, NULL);
    }
    buf->num_queued_frames = 0;
    buf->queued_frames_alloc = 0;
    buf->max_queued_frames = max_queued_frames;
    buf->queued_frames = NULL;
    return !buf->queued_frames ? AVERROR(ENOMEM) : 0;
}

static inline int get_fifo_size(AVFrameFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);
    int ret = buf->num_queued_frames;
    pthread_mutex_unlock(&buf->lock);
    return ret;
}

static inline int push_to_fifo(AVFrameFIFO *buf, AVFrame *f)
{
    int err = 0;
    pthread_mutex_lock(&buf->cond_lock_out);
    pthread_mutex_lock(&buf->lock);

    if ((buf->max_queued_frames > 0) &&
        (buf->num_queued_frames > buf->max_queued_frames)) {
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_out, &buf->cond_lock_out);
        pthread_mutex_lock(&buf->lock);
    }

    unsigned int oalloc = buf->queued_frames_alloc;
    AVFrame **fq = av_fast_realloc(buf->queued_frames, &buf->queued_frames_alloc,
                                   sizeof(AVFrame *)*(buf->num_queued_frames + 1));
    if (!fq) {
        buf->queued_frames_alloc = oalloc;
        err = AVERROR(ENOMEM);
        goto fail;
    }

    buf->queued_frames = fq;
    buf->queued_frames[buf->num_queued_frames++] = f;

    pthread_cond_signal(&buf->cond_in);

fail:
    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_out);

    return err;
}

static inline AVFrame *pop_from_fifo(AVFrameFIFO *buf)
{
    pthread_mutex_lock(&buf->cond_lock_in);
    pthread_mutex_lock(&buf->lock);

    if (!buf->num_queued_frames) {
        pthread_mutex_unlock(&buf->lock);
        pthread_cond_wait(&buf->cond_in, &buf->cond_lock_in);
        pthread_mutex_lock(&buf->lock);
    }

    buf->num_queued_frames--;
    assert(buf->num_queued_frames >= 0);

    AVFrame *rf = buf->queued_frames[0];

    memmove(&buf->queued_frames[0], &buf->queued_frames[1],
            buf->num_queued_frames*sizeof(*buf->queued_frames));

    if (buf->max_queued_frames > 0)
        pthread_cond_signal(&buf->cond_out);

    pthread_mutex_unlock(&buf->lock);
    pthread_mutex_unlock(&buf->cond_lock_in);

    return rf;
}

static inline void free_fifo(AVFrameFIFO *buf)
{
    pthread_mutex_lock(&buf->lock);
    if (buf->num_queued_frames)
        for (int i = 0; i < buf->num_queued_frames; i++)
            av_frame_free(&buf->queued_frames[i]);
    av_freep(&buf->queued_frames);
    pthread_mutex_unlock(&buf->lock);
}
