#ifndef COCO_H
#define COCO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COCO_OK                    0
#define COCO_ERROR                -1
#define COCO_ERROR_NOMEM          -2
#define COCO_ERROR_CANCELLED      -3
#define COCO_ERROR_WOULD_BLOCK    -4
#define COCO_ERROR_TIMEOUT        -5

typedef struct coco_sched coco_sched_t;
typedef struct coco_coro coco_coro_t;
typedef struct coco_timer coco_timer_t;

typedef void (*coco_func_t)(void *arg);
typedef void (*coco_timer_handler_t)(void *arg);

typedef struct {
    int         flags;
    int         stack_size;
    int         prio;
    const char *name;
} coco_go_opts_t;

int  coco_global_sched_start(int num_workers);
void coco_global_sched_wait(void);
void coco_global_sched_stop(void);

coco_coro_t *coco_go(coco_func_t func, void *arg);
coco_coro_t *coco_go_with_opts(coco_func_t func, void *arg, const coco_go_opts_t *opts);
coco_coro_t *coco_self(void);
int  coco_cancel(coco_coro_t *coro);
coco_sched_t *coco_sched_get_current(void);

ssize_t coco_read(int fd, void *buf, size_t len);
ssize_t coco_write(int fd, const void *buf, size_t len);

coco_timer_t *coco_timer(uint32_t ms, coco_timer_handler_t handler, void *arg);
void coco_timer_cancel(coco_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif /* COCO_H */
