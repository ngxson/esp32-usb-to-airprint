#include "shared_buf.h"

#include <pthread.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *s_data;
static size_t s_size;
static sbuf_user_t s_writer;        // holds write access
static sbuf_user_t s_owner;         // whose content the buffer holds
static size_t s_len;
static int s_readers;

void sbuf_init(uint8_t *data, size_t size)
{
    pthread_mutex_lock(&s_lock);
    s_data = data;
    s_size = data ? size : 0;
    s_writer = s_owner = SBUF_NONE;
    s_len = 0;
    pthread_mutex_unlock(&s_lock);
}

size_t sbuf_size(void)
{
    return s_size;
}

uint8_t *sbuf_acquire(sbuf_user_t user, size_t *size)
{
    uint8_t *p = NULL;
    pthread_mutex_lock(&s_lock);
    if (s_data && s_writer == SBUF_NONE && s_readers == 0) {
        s_writer = user;
        s_owner = SBUF_NONE;
        s_len = 0;
        p = s_data;
        *size = s_size;
    }
    pthread_mutex_unlock(&s_lock);
    return p;
}

void sbuf_release(sbuf_user_t user, size_t content_len)
{
    pthread_mutex_lock(&s_lock);
    if (s_writer == user) {
        s_writer = SBUF_NONE;
        s_owner = content_len ? user : SBUF_NONE;
        s_len = content_len <= s_size ? content_len : 0;
    }
    pthread_mutex_unlock(&s_lock);
}

const uint8_t *sbuf_read_begin(sbuf_user_t user, size_t *len)
{
    const uint8_t *p = NULL;
    pthread_mutex_lock(&s_lock);
    if (s_writer == SBUF_NONE && s_owner == user && s_len) {
        s_readers++;
        p = s_data;
        *len = s_len;
    }
    pthread_mutex_unlock(&s_lock);
    return p;
}

void sbuf_read_end(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_readers > 0)
        s_readers--;
    pthread_mutex_unlock(&s_lock);
}

size_t sbuf_content(sbuf_user_t user)
{
    pthread_mutex_lock(&s_lock);
    size_t n = s_writer == SBUF_NONE && s_owner == user ? s_len : 0;
    pthread_mutex_unlock(&s_lock);
    return n;
}
