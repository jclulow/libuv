/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/*
 * This file is private to libuv. It provides common functionality to both
 * Windows and Unix backends.
 */

#ifndef UV_CROSSQ_H_
#define UV_CROSSQ_H_

#include <assert.h>
#include <stddef.h>

#if defined(_MSC_VER) && _MSC_VER < 1600
# include "stdint-msvc2008.h"
#else
# include <stdint.h>
#endif

/*#include "uv.h"*/
#include "queue.h"

#ifndef _WIN32

typedef enum uv__crossq_ent_flags {
  UV__CROSSQ_ENT_NOREPLY   = 0x01,
  UV__CROSSQ_ENT_COMPLETE  = 0x02
} uv__crossq_ent_flags_t;

typedef enum uv__crossq_flags {
  UV__CROSSQ_ACTIVE        = 0x01,
  UV__CROSSQ_SHUTTING_DOWN = 0x02
} uv__crossq_flags_t;

#else /* _WIN32 */

typedef unsigned int uv__crossq_ent_flags_t;
# define UV__CROSSQ_ENT_NOREPLY   0x01
# define UV__CROSSQ_ENT_COMPLETE  0x02

typedef unsigned int uv__crossq_flags_t;
# define UV__CROSSQ_ACTIVE        0x01
# define UV__CROSSQ_SHUTTING_DOWN 0x02

#endif

typedef struct uv__crossq_ent {
  uv_mutex_t cqe_lock;
  uv_cond_t cqe_cv;
  uv__crossq_ent_flags_t cqe_flags;
  QUEUE cqe_link;
  uv_cross_cb cqe_func;
  void *cqe_in;
  void **cqe_out;
} uv__crossq_ent_t;

typedef struct uv__crossq {
  uv_mutex_t cq_lock;
  uv_async_t cq_async;
  uv__crossq_flags_t cq_flags;
  unsigned long cq_thread_id;
  QUEUE cq_queue;
} uv__crossq_t;

int uv__cross_call_init(uv_loop_t *);
void uv__cross_call_fini(uv_loop_t *);

#endif /* UV_CROSSQ_H_ */
