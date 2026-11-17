#ifndef JC2_BUFFER_SERVICE_H
#define JC2_BUFFER_SERVICE_H

#include "service_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Requested access flags for acquire_buffer. */
#define JC2_BUFFER_READ  1u
#define JC2_BUFFER_WRITE 2u

/* A borrowed view of an object's memory. The provider owns `token`; the
 * consumer must call release(token) once, after the final use of `data`. */
typedef struct JC2_BufferView {
    uint8_t* data;
    size_t size;
    uint32_t flags;       /* actual access supported by the view */
    void* token;          /* provider-private lifetime token */
    void (*release)(void* token);
} JC2_BufferView;

/* 1 = acquired, 0 = not a buffer of this provider, -1 = provider error. */
typedef int (*JC2_BufferAcquireFn)(
    JC2_ValueHandle value,
    uint32_t requested,
    JC2_BufferView* out
);

#ifdef __cplusplus
}
#endif

#endif // JC2_BUFFER_SERVICE_H
