#include <stdlib.h>
#include <string.h>
#include "ringbuffer.h"

int ring_init(RingBuffer *rb, size_t frames, size_t channels) {
  rb->capacity = frames;
  rb->channels = channels;
  rb->readPos = rb->writePos = rb->count = 0;
  rb->data = malloc(frames * channels * sizeof(float));
  if (!rb->data) return -1;
  pthread_mutex_init(&rb->mu, NULL);
  return 0;
}

void ring_free(RingBuffer *rb) {
  pthread_mutex_destroy(&rb->mu);
  free(rb->data);
  rb->data = NULL;
  rb->capacity = rb->count = rb->readPos = rb->writePos = 0;
}

size_t ring_space(const RingBuffer *rb) {
  return rb->capacity - rb->count;
}

size_t ring_available(const RingBuffer *rb) {
  return rb->count;
}

size_t ring_write(RingBuffer *rb, const float *in, size_t frames) {
  pthread_mutex_lock(&rb->mu);
  size_t written = 0;
  while (written < frames && rb->count < rb->capacity) {
    for (size_t c = 0; c < rb->channels; c++) {
      rb->data[(rb->writePos * rb->channels + c)] = in[written * rb->channels + c];
    }
    rb->writePos = (rb->writePos + 1) % rb->capacity;
    rb->count++;
    written++;
  }
  pthread_mutex_unlock(&rb->mu);
  return written;
}

size_t ring_read(RingBuffer *rb, float *out, size_t frames) {
  pthread_mutex_lock(&rb->mu);
  size_t read = 0;
  while (read < frames && rb->count > 0) {
    for (size_t c = 0; c < rb->channels; c++) {
      out[read * rb->channels + c] = 
         rb->data[(rb->readPos * rb->channels + c)];
    }
    rb->readPos = (rb->readPos + 1) % rb->capacity;
    rb->count--;
    read++;
  }
  pthread_mutex_unlock(&rb->mu);
  return read;
}
