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

#include "uv.h"
#include "uv-common.h"
#include "uv-crossq.h"

#include <stdio.h>
#include <assert.h>
#include <stddef.h> /* NULL */
#include <stdlib.h> /* malloc */
#include <string.h> /* memset */

/* EAI_* constants. */
#if !defined(_WIN32)
# include <sys/types.h>
# include <sys/socket.h>
# include <netdb.h>
#endif

static void uv__cross_call_async_cb(uv_async_t *, int);
static void uv__cross_call_flushq(uv_loop_t*, uv__crossq_t*);
static void uv__cross_call_dispatch(uv_loop_t*, uv__crossq_ent_t*);

int uv_cross_post(uv_loop_t *loop, uv_cross_cb func, void *in) {
  uv__crossq_t *cq = loop->crossq;
  uv__crossq_ent_t* cqe;
  int ret;

  if ((cqe = calloc(1, sizeof (*cqe))) == NULL)
    return -ENOMEM;

  assert(0 == uv_mutex_init(&cqe->cqe_lock));
  assert(0 == uv_cond_init(&cqe->cqe_cv));

  cqe->cqe_flags = UV__CROSSQ_ENT_NOREPLY;
  cqe->cqe_in = in;
  cqe->cqe_out = NULL;

  uv_mutex_lock(&cq->cq_lock);
  if (cq->cq_flags & UV__CROSSQ_SHUTTING_DOWN) {
    uv_mutex_unlock(&cq->cq_lock);
    free(cqe);
    return -1;
  }
  if ((ret = uv_async_send(&cq->cq_async)) != 0) {
    /*
     * If we cannot signal the event loop thread, bail out without adding
     * our request to the queue.
     */
    uv_mutex_unlock(&cq->cq_lock);
    free(cqe);
    return ret;
  }
  QUEUE_INSERT_TAIL(&cq->cq_queue, &cqe->cqe_link);
  uv_mutex_unlock(&cq->cq_lock);

  return uv_async_send(&cq->cq_async);
}


int uv_cross_call(uv_loop_t* loop, uv_cross_cb func, void *in, void **out) {
  uv__crossq_t *cq = loop->crossq;
  uv__crossq_ent_t cqe;
  int ret;

  /*
   * We cannot block in the event loop thread, as this would cause us to
   * deadlock with ourselves.
   */
  assert(cq->cq_thread_id != uv_thread_self());

  assert(0 == uv_mutex_init(&cqe.cqe_lock));
  assert(0 == uv_cond_init(&cqe.cqe_cv));

  cqe.cqe_flags = 0;
  cqe.cqe_in = in;
  cqe.cqe_out = out;

  uv_mutex_lock(&cq->cq_lock);
  if (cq->cq_flags & UV__CROSSQ_SHUTTING_DOWN) {
    uv_mutex_unlock(&cq->cq_lock);
    return -1;
  }
  if ((ret = uv_async_send(&cq->cq_async)) != 0) {
    /*
     * If we cannot signal the event loop thread, bail out without adding
     * our request to the queue.
     */
    uv_mutex_unlock(&cq->cq_lock);
    return ret;
  }
  QUEUE_INSERT_TAIL(&cq->cq_queue, &cqe.cqe_link);
  uv_mutex_unlock(&cq->cq_lock);

  /*
   * Wait on our request to be completed in the event loop thread:
   */
  uv_mutex_lock(&cqe.cqe_lock);
  while ((cqe.cqe_flags & UV__CROSSQ_ENT_COMPLETE) == 0) {
    uv_cond_wait(&cqe.cqe_cv, &cqe.cqe_lock);
  }
  uv_mutex_unlock(&cqe.cqe_lock);

  /*
   * Clean up:
   */
  uv_mutex_destroy(&cqe.cqe_lock);
  uv_cond_destroy(&cqe.cqe_cv);
  return 0;
}


int uv__cross_call_init(uv_loop_t *loop) {
  uv__crossq_t* cq;
  if ((cq = calloc(1, sizeof (*cq))) == NULL)
    return -ENOMEM;

  assert(0 == uv_mutex_init(&cq->cq_lock));
  assert(0 == uv_async_init(loop, &cq->cq_async, uv__cross_call_async_cb));
  cq->cq_thread_id = uv_thread_self();
  cq->cq_flags = UV__CROSSQ_ACTIVE;
  QUEUE_INIT(&cq->cq_queue);

  loop->crossq = cq;
  return 0;
}


void uv__cross_call_fini(uv_loop_t *loop) {
  uv__crossq_t* cq;

  if ((cq = loop->crossq) == NULL)
    return;

  uv_mutex_lock(&cq->cq_lock);
  assert(cq->cq_flags == UV__CROSSQ_ACTIVE);
  /*
   * Mark the queue as closing, so that we refuse to enqueue more
   * execution requests:
   */
  cq->cq_flags |= UV__CROSSQ_SHUTTING_DOWN;
  /*
   * Finish servicing all previously enqueued requests prior to queue
   * destruction:
   */
  uv__cross_call_flushq(loop, cq);
  assert(QUEUE_EMPTY(&cq->cq_queue));
  /*
   * XXX what to do here...
  uv_close(&cq->cq_async);
   */

  cq->cq_flags &= ~(UV__CROSSQ_ACTIVE);
  uv_mutex_unlock(&cq->cq_lock);

  /*
   * Clean up:
   */
  uv_mutex_destroy(&cq->cq_lock);
  free(cq);
  loop->crossq = NULL;
}


static void uv__cross_call_async_cb(uv_async_t *handle, int status) {
  uv__crossq_t *cq = handle->data;

  uv_mutex_lock(&cq->cq_lock);
  assert(cq->cq_flags & UV__CROSSQ_ACTIVE);

  uv__cross_call_flushq(handle->loop, cq);

  uv_mutex_unlock(&cq->cq_lock);
}


static void uv__cross_call_flushq(uv_loop_t* loop, uv__crossq_t* cq) {
  /*
   * We must be run on the event loop thread:
   */
  assert(cq->cq_thread_id == uv_thread_self());
  assert(cq->cq_flags & UV__CROSSQ_ACTIVE);

  while (!QUEUE_EMPTY(&cq->cq_queue)) {
    QUEUE *q = QUEUE_HEAD(&cq->cq_queue);
    QUEUE_REMOVE(q);
    
    /*
     * Dispatch:
     */
    uv_mutex_unlock(&cq->cq_lock);
    uv__cross_call_dispatch(loop, QUEUE_DATA(q, uv__crossq_ent_t, cqe_link));
    uv_mutex_lock(&cq->cq_lock);
  }
}


static void uv__cross_call_dispatch(uv_loop_t* loop, uv__crossq_ent_t* cqe) {
  uv_mutex_lock(&cqe->cqe_lock);

  cqe->cqe_func(loop, cqe->cqe_in, cqe->cqe_out);

  /* XXX */

  if (cqe->cqe_flags & UV__CROSSQ_ENT_NOREPLY) {
    /*
     * We are responsible for freeing this structure:
     */
    uv_mutex_unlock(&cqe->cqe_lock);
    free(cqe);
    return;
  }

  uv_cond_broadcast(&cqe->cqe_cv);
  uv_mutex_unlock(&cqe->cqe_lock);
}
