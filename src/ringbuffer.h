#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stddef.h>
#include <pthread.h>

typedef struct {
  float *data;
  size_t capacity;
  size_t channels;
  size_t readPos;
  size_t writePos;
  size_t count;
  pthread_mutex_t mu;
} RingBuffer;

int ring_init(RingBuffer *rb, size_t frames, size_t channels);
void ring_free(RingBuffer *rb);
void ring_clear(RingBuffer *rb);
size_t ring_write(RingBuffer *rb, const float *in, size_t frames);
size_t ring_read(RingBuffer *rb, float *out, size_t frames);
size_t ring_space(const RingBuffer *rb);
size_t ring_available(const RingBuffer *rb);
#endif
