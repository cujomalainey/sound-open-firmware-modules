//https://embedjournal.com/implementing-circular-buffer-embedded-c/
#include <stdint.h>

typedef struct {
    uint8_t * const buffer;
    int head;
    int tail;
    const int maxLen;
} circBuf_t;

#define CIRCBUF_DEF(x,y)          \
    uint8_t x##_dataSpace[y];     \
    circBuf_t x = {               \
        .buffer = x##_dataSpace,  \
        .head = 0,                \
        .tail = 0,                \
        .maxLen = y               \
    }

int circ_buf_pop(circBuf_t *c, uint8_t *data);
int circ_buf_push(circBuf_t *c, uint8_t data);
