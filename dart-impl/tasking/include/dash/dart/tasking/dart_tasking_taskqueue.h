/*
 * dart_tasking_taskqueue.h
 *
 *  Created on: Jan 16, 2017
 *      Author: joseph
 */

#ifndef DART_TASKING_TASKQUEUE_H_
#define DART_TASKING_TASKQUEUE_H_

#include <pthread.h>

#include <dash/dart/tasking/dart_tasking_priv.h>

/**
 * Initialize a task queue.
 */
void
dart_tasking_taskqueue_init(dart_taskqueue_t *tq);

/**
 * Pop a task from the HEAD of the task queue.
 */
dart_task_t *
dart_tasking_taskqueue_pop(dart_taskqueue_t *tq);

/**
 * Push a task to the HEAD of the task queue.
 */
void
dart_tasking_taskqueue_push(dart_taskqueue_t *tq, dart_task_t *task);

/**
 * Pop a task from the back of the task queue.
 * Used to steal tasks from other threads.
 */
dart_task_t *
dart_tasking_taskqueue_popback(dart_taskqueue_t *tq);

/**
 * Check whether the task queue is empty.
 *
 * \return   0 if the task queue is not empty.
 *         !=0 if the task queue is empty.
 */
static inline int
dart_tasking_taskqueue_isempty(const dart_taskqueue_t *tq)
{
  return (tq->head == NULL);
}

/**
 * Move the tasks enqueued in \c src to the queue dst.
 * Tasks are prepended at the destination queue.
 */
dart_ret_t
dart_tasking_taskqueue_move(dart_taskqueue_t *dst, dart_taskqueue_t *src);

/**
 * Finalize a task queue, releasing held resources.
 */
void
dart_tasking_taskqueue_finalize(dart_taskqueue_t *tq);


#endif /* DART_TASKING_TASKQUEUE_H_ */
