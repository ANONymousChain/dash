
#include <dash/dart/base/logging.h>
#include <dash/dart/base/atomic.h>
#include <dash/dart/if/dart_tasking.h>
#include <dash/dart/if/dart_active_messages.h>
#include <dash/dart/base/hwinfo.h>
#include <dash/dart/tasking/dart_tasking_priv.h>
#include <dash/dart/tasking/dart_tasking_ayudame.h>
#include <dash/dart/tasking/dart_tasking_taskqueue.h>
#include <dash/dart/tasking/dart_tasking_tasklist.h>
#include <dash/dart/tasking/dart_tasking_taskqueue.h>
#include <dash/dart/tasking/dart_tasking_datadeps.h>
#include <dash/dart/tasking/dart_tasking_remote.h>

#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>



// true if threads should process tasks. Set to false to quit parallel processing
static bool parallel = false;

static int num_threads;

static bool initialized = false;

static pthread_key_t tpd_key;

static uint64_t phase_bound = 0;

typedef struct {
  int           thread_id;
} tpd_t;

static pthread_cond_t  task_avail_cond   = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t thread_pool_mutex = PTHREAD_MUTEX_INITIALIZER;

static dart_task_t *task_recycle_list     = NULL;
static dart_task_t *task_free_list        = NULL;
static pthread_mutex_t task_recycle_mutex = PTHREAD_MUTEX_INITIALIZER;

static dart_thread_t *thread_pool;

// a dummy task that serves as a root task for all other tasks
static dart_task_t root_task = {
    .next = NULL,
    .prev = NULL,
    .fn   = NULL,
    .data = NULL,
    .data_size = 0,
    .unresolved_deps = 0,
    .successor = NULL,
    .parent = NULL,
    .remote_successor = NULL,
    .num_children = 0,
    .phase  = 0,
    .state  = DART_TASK_ROOT};


static void wait_for_work()
{
  pthread_mutex_lock(&thread_pool_mutex);
  pthread_cond_wait(&task_avail_cond, &thread_pool_mutex);
  pthread_mutex_unlock(&thread_pool_mutex);
}

static void destroy_tsd(void *tsd)
{
  free(tsd);
}

static inline
void set_current_task(dart_task_t *t)
{
  thread_pool[((tpd_t*)pthread_getspecific(tpd_key))->thread_id].current_task = t;
}

static inline
dart_task_t * get_current_task()
{
  return thread_pool[((tpd_t*)pthread_getspecific(tpd_key))->thread_id].current_task;
}

static
dart_task_t * next_task(dart_thread_t *thread)
{
  dart_task_t *task = dart_tasking_taskqueue_pop(&thread->queue);
  if (task == NULL) {
    // try to steal from another thread, round-robbing starting to the right
    for (int i  = (thread->thread_id + 1) % num_threads;
             i != thread->thread_id;
             i  = (i + 1) % num_threads) {
      task = dart_tasking_taskqueue_popback(&thread_pool[i].queue);
      if (task != NULL) {
        DART_LOG_DEBUG("Stole task %p from thread %i", task, i);
        break;
      }
    }
  }
  return task;
}

static inline
dart_task_t * create_task(void (*fn) (void *), void *data, size_t data_size)
{
  dart_task_t *task = NULL;
  if (task_free_list != NULL) {
    pthread_mutex_lock(&task_recycle_mutex);
    if (task_free_list != NULL) {
      DART_STACK_POP(task_free_list, task);
//      task = task_free_list;
//      task_free_list = task->next;
    }
    pthread_mutex_unlock(&task_recycle_mutex);
  } else {
    pthread_mutex_unlock(&task_recycle_mutex);
    task = calloc(1, sizeof(dart_task_t));
    dart_mutex_init(&task->mutex);
  }

  if (data_size) {
    task->data_size  = data_size;
    task->data       = malloc(data_size);
    memcpy(task->data, data, data_size);
  } else {
    task->data       = data;
    task->data_size  = 0;
  }
  task->fn           = fn;
  task->num_children = 0;
  task->parent       = get_current_task();
  task->state        = DART_TASK_CREATED;
  task->phase        = task->parent->phase;
  task->has_ref      = false;
  return task;
}

static inline
void destroy_task(dart_task_t *task)
{
  if (task->data_size) {
    free(task->data);
  }
  // reset some of the fields
  // IMPORTANT: the state may not be rewritten!
  task->data             = NULL;
  task->data_size        = 0;
  task->fn               = NULL;
  task->parent           = NULL;
  task->phase            = 0;
  task->prev             = NULL;
  task->remote_successor = NULL;
  task->successor        = NULL;
  task->state            = DART_TASK_DESTROYED;
  task->has_ref          = false;
  pthread_mutex_lock(&task_recycle_mutex);
  DART_STACK_PUSH(task_recycle_list, task);
//  task->next = task_recycle_list;
//  task_recycle_list = task;
  pthread_mutex_unlock(&task_recycle_mutex);
//  free(task);
}


/**
 * Execute the given task.
 */
static
void handle_task(dart_task_t *task)
{
  if (task != NULL)
  {
    DART_LOG_INFO("Thread %i executing task %p", dart__tasking__thread_num(), task);

    // save current task and set to new task
    dart_task_t *current_task = get_current_task();
    set_current_task(task);

    dart_task_action_t fn = task->fn;
    void *data = task->data;

    dart_mutex_lock(&(task->mutex));
    task->state = DART_TASK_RUNNING;
    dart_mutex_unlock(&(task->mutex));

    DART_LOG_DEBUG("Invoking task %p (fn:%p data:%p)", task, task->fn, task->data);
    //invoke the task function
    fn(data);
    DART_LOG_DEBUG("Done with task %p (fn:%p data:%p)", task, fn, data);

    // Implicit wait for child tasks
    dart__tasking__task_complete();

    // we need to lock the task shortly here
    // to allow for atomic check and update
    // of remote successors in dart_tasking_datadeps_handle_remote_task
    dart_mutex_lock(&(task->mutex));
    task->state = DART_TASK_TEARDOWN;
    dart_tasking_datadeps_release_local_task(task);
    task->state = DART_TASK_FINISHED;
    dart_mutex_unlock(&(task->mutex));

    // let the parent know that we are done
    int32_t nc = DART_DEC_AND_FETCH32(&task->parent->num_children);
    DART_LOG_DEBUG("Parent %p has %i children left\n", task->parent, nc);

    // clean up
    if (!task->has_ref){
      // only destroy the task if there are no references outside
      // referenced tasks will be destroyed in task_wait
      destroy_task(task);
    }

    // return to previous task
    set_current_task(current_task);
  }
}


static
void* thread_main(void *data)
{
  tpd_t *tpd = (tpd_t*)data;
  pthread_setspecific(tpd_key, tpd);

  dart_thread_t *thread = &thread_pool[tpd->thread_id];

  set_current_task(&root_task);

  // enter work loop
  while (parallel) {
    // look for incoming remote tasks and responses
    dart_tasking_remote_progress();
    dart_task_t *task = next_task(thread);
    handle_task(task);
    // only go to sleep if no tasks are in flight
    if (DART_FETCH32(&(root_task.num_children)) == 0) {
      if (dart__tasking__thread_num() == dart__tasking__num_threads() - 1)
      {
        // the last thread is responsible for ensuring progress on the
        // message queue even if all others are sleeping
        dart_tasking_remote_progress();
      } else {
        wait_for_work();
      }
    }
  }

  DART_LOG_INFO("Thread %i exiting", dart__tasking__thread_num());

  return NULL;
}

static
void dart_thread_init(dart_thread_t *thread, int threadnum)
{
  thread->thread_id = threadnum;
  thread->current_task = NULL;
  dart_tasking_taskqueue_init(&thread->queue);
  dart_tasking_taskqueue_init(&thread->defered_queue);
}

static
void dart_thread_finalize(dart_thread_t *thread)
{
  thread->thread_id = -1;
  thread->current_task = NULL;
  dart_tasking_taskqueue_finalize(&thread->queue);
  dart_tasking_taskqueue_finalize(&thread->defered_queue);
}


dart_thread_t *
dart__tasking_current_thread()
{
  return &thread_pool[dart__tasking__thread_num()];
}


dart_ret_t
dart__tasking__init()
{
  if (initialized) {
    DART_LOG_ERROR("DART tasking subsystem can only be initialized once!");
    return DART_ERR_INVAL;
  }
  dart_hwinfo_t hw;
  dart_hwinfo(&hw);
  if (hw.num_cores > 0) {
    num_threads = hw.num_cores * hw.max_threads;
  } else {
    DART_LOG_INFO("Failed to get number of cores! Playing it safe with 2 threads...");
    num_threads = 2;
  }

  DART_LOG_INFO("Using %i threads", num_threads);

  dart_amsg_init();

  // keep threads running
  parallel = true;

  // set up the active message queue
  dart_tasking_datadeps_init();

  // initialize all task threads before creating them
  thread_pool = malloc(sizeof(dart_thread_t) * num_threads);
  for (int i = 0; i < num_threads; i++)
  {
    dart_thread_init(&thread_pool[i], i);
  }

  pthread_key_create(&tpd_key, &destroy_tsd);
  // set master thread id
  tpd_t *tpd = malloc(sizeof(tpd_t));
  tpd->thread_id = 0;
  pthread_setspecific(tpd_key, tpd);

  set_current_task(&root_task);
  for (int i = 1; i < num_threads; i++)
  {
    tpd = malloc(sizeof(tpd_t));
    tpd->thread_id = i; // 0 is reserved for master thread
    int ret = pthread_create(&thread_pool[i].pthread, NULL, &thread_main, tpd);
    if (ret != 0) {
      DART_LOG_ERROR("Failed to create thread %i of %i!", i, num_threads);
    }
  }

#ifdef DART_ENABLE_AYUDAME
  dart__tasking__ayudame_init();
#endif // DART_ENABLE_AYUDAME

  initialized = true;

  return DART_OK;
}

int
dart__tasking__thread_num()
{
  return (initialized ? ((tpd_t*)pthread_getspecific(tpd_key))->thread_id : 0);
}

int
dart__tasking__num_threads()
{
  return (initialized ? num_threads : 1);
}

uint64_t
dart__tasking__phase_bound()
{
  return phase_bound;
}

void
dart__tasking__enqueue_runnable(dart_task_t *task)
{
  dart_thread_t *thread = &thread_pool[dart__tasking__thread_num()];
  dart_taskqueue_t *q = &thread->queue;
  if (task->phase > phase_bound) {
    // if the task's phase is outside the phase bound we defer it
    q = &thread->defered_queue;
  }
  dart_tasking_taskqueue_push(q, task);
}

dart_ret_t
dart__tasking__create_task(
    void           (*fn) (void *),
    void            *data,
    size_t           data_size,
    dart_task_dep_t *deps,
    size_t           ndeps)
{
  dart_task_t *task = create_task(fn, data, data_size);

  int32_t nc = DART_INC_AND_FETCH32(&task->parent->num_children);
  DART_LOG_DEBUG("Parent %p now has %i children", task->parent, nc);

  dart_tasking_datadeps_handle_task(task, deps, ndeps);

  if (task->unresolved_deps == 0) {
    dart__tasking__enqueue_runnable(task);
  }

  return DART_OK;
}

dart_ret_t
dart__tasking__create_task_handle(
    void           (*fn) (void *),
    void            *data,
    size_t           data_size,
    dart_task_dep_t *deps,
    size_t           ndeps,
    dart_taskref_t  *ref)
{
  dart_task_t *task = create_task(fn, data, data_size);
  task->has_ref = true;

  int32_t nc = DART_INC_AND_FETCH32(&task->parent->num_children);
  DART_LOG_DEBUG("Parent %p now has %i children", task->parent, nc);

  dart_tasking_datadeps_handle_task(task, deps, ndeps);

  if (task->unresolved_deps == 0) {
    dart__tasking__enqueue_runnable(task);
  }

  *ref = task;

  return DART_OK;
}


dart_ret_t
dart__tasking__task_complete()
{
  dart_thread_t *thread = &thread_pool[dart__tasking__thread_num()];

  if (thread->current_task == &(root_task) && thread->thread_id != 0) {
    DART_LOG_ERROR("dart__tasking__task_complete() called on ROOT task "
                   "only valid on MASTER thread!");
    return DART_ERR_INVAL;
  }


  if (thread->current_task == &(root_task)) {
    // once again make sure all incoming requests are served
    dart_tasking_remote_progress_blocking();
    // release unhandled remote dependencies
    dart_tasking_datadeps_release_unhandled_remote();
    // release defered tasks
    phase_bound = thread->current_task->phase;
    dart_tasking_taskqueue_move(&thread->queue, &thread->defered_queue);
  }

  // 1) wake up all threads (might later be done earlier)
  pthread_cond_broadcast(&task_avail_cond);


  // 2) start processing ourselves
  dart_task_t *task = get_current_task();
  while (DART_FETCH32(&(task->num_children)) > 0) {
    // a) look for incoming remote tasks and responses
    dart_tasking_remote_progress();
    // b) process our tasks
    dart_task_t *task = next_task(thread);
    handle_task(task);
  }

  // 3) clean up if this was the root task and thus no other tasks are running
  if (thread->current_task == &(root_task)) {
    dart_tasking_datadeps_reset();
    // recycled tasks can now be used again
    pthread_mutex_lock(&task_recycle_mutex);
    task_free_list = task_recycle_list;
    task_recycle_list = NULL;
    pthread_mutex_unlock(&task_recycle_mutex);
  }

  return DART_OK;
}


dart_ret_t
dart__tasking__task_wait(dart_taskref_t *tr)
{
  dart_thread_t *thread = &thread_pool[dart__tasking__thread_num()];

  if (tr == NULL || *tr == NULL || (*tr)->state == DART_TASK_DESTROYED) {
    return DART_ERR_INVAL;
  }

  // the thread just contributes to the execution
  // of available tasks until the task waited on finishes
  while ((*tr)->state != DART_TASK_FINISHED) {
    dart_tasking_remote_progress();
    dart_task_t *task = next_task(thread);
    handle_task(task);
  }

  destroy_task(*tr);
  *tr = NULL;

  return DART_OK;
}



dart_ret_t
dart__tasking__phase()
{
  if (dart__tasking__thread_num() != 0) {
    DART_LOG_ERROR("Switching phases can only be done by the master thread!");
    return DART_ERR_INVAL;
  }
//  dart_barrier(DART_TEAM_ALL);
  dart_tasking_remote_progress();
  dart_tasking_datadeps_end_phase(root_task.phase);
  root_task.phase++;
  DART_LOG_INFO("Starting task phase %li\n", root_task.phase);
  return DART_OK;
}

dart_taskref_t
dart__tasking__current_task()
{
  return thread_pool[dart__tasking__thread_num()].current_task;
}

dart_ret_t
dart__tasking__fini()
{
  if (!initialized) {
    DART_LOG_ERROR("DART tasking subsystem has not been initialized!");
    return DART_ERR_INVAL;
  }
  int i;

  DART_LOG_DEBUG("dart__tasking__fini(): Tearing down task subsystem");

  parallel = false;

  // wake up all threads waiting for work
  pthread_cond_broadcast(&task_avail_cond);

  // wait for all threads to finish
  for (i = 1; i < num_threads; i++) {
    pthread_join(thread_pool[i].pthread, NULL);
    dart_thread_finalize(&thread_pool[i]);
  }

#ifdef DART_ENABLE_AYUDAME
  dart__tasking__ayudame_fini();
#endif // DART_ENABLE_AYUDAME

  dart_task_t *task = task_recycle_list;
  while (task != NULL) {
    dart_task_t *tmp = task;
    task = task->next;
    tmp->next = NULL;
    free(tmp);
  }
  task_recycle_list = NULL;

  initialized = false;
  DART_LOG_DEBUG("dart__tasking__fini(): Finished with tear-down");

  return DART_OK;
}
