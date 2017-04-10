
#include <dash/dart/base/assert.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/mutex.h>
#include <dash/dart/base/atomic.h>
#include <dash/dart/if/dart_tasking.h>
#include <dash/dart/tasking/dart_tasking_priv.h>
#include <dash/dart/tasking/dart_tasking_tasklist.h>
#include <dash/dart/tasking/dart_tasking_datadeps.h>
#include <dash/dart/tasking/dart_tasking_remote.h>
#include <dash/dart/tasking/dart_tasking_taskqueue.h>

#include <stdbool.h>

#define DART_DEPHASH_SIZE 1024

/**
 * Management of task data dependencies using a hash map that maps pointers to tasks.
 * The hash map implementation is taken from dart_segment.
 * The hash uses the absolute local address stored in the gptr since that is used
 * throughout the task handling code.
 */


#define IS_OUT_DEP(taskdep) \
  (((taskdep).type == DART_DEP_OUT || (taskdep).type == DART_DEP_INOUT))

#define IS_ACTIVE_TASK(task) \
  ((task)->state == DART_TASK_RUNNING || (task)->state == DART_TASK_CREATED)

typedef struct dart_dephash_elem {
  struct dart_dephash_elem *next;
  union taskref             task;
  dart_task_dep_t           taskdep;
  uint64_t                  phase;
} dart_dephash_elem_t;

static dart_dephash_elem_t *local_deps[DART_DEPHASH_SIZE];
static dart_dephash_elem_t *freelist_head = NULL;
static dart_mutex_t         local_deps_mutex;

static dart_dephash_elem_t *unhandled_remote_deps = NULL;
static dart_mutex_t         unhandled_remote_mutex;

static dart_mutex_t         deferred_remote_mutex;
static dart_dephash_elem_t *deferred_remote_releases = NULL;


static dart_ret_t
release_remote_dependencies(dart_task_t *task);

static void
dephash_recycle_elem(dart_dephash_elem_t *elem);

static inline int hash_gptr(dart_gptr_t gptr)
{
  /**
   * Use the upper 61 bit of the pointer since we assume that pointers
   * are 8-byte aligned.
   */
  uint64_t offset = gptr.addr_or_offs.offset;
  offset >>= 3;
  // use triplet (7, 11, 10), consider adding (21,17,48)
  // proposed by Marsaglia
  return ((offset ^ (offset >> 7) ^ (offset >> 11) ^ (offset >> 17))
              % DART_DEPHASH_SIZE);
  //return ((gptr.addr_or_offs.offset >> 3) % DART_DEPHASH_SIZE);
}

/**
 * Initialize the data dependency management system.
 */
dart_ret_t dart_tasking_datadeps_init()
{
  memset(local_deps, 0, sizeof(dart_dephash_elem_t*) * DART_DEPHASH_SIZE);

  dart_mutex_init(&local_deps_mutex);
  dart_mutex_init(&unhandled_remote_mutex);
  dart_mutex_init(&deferred_remote_mutex);

  return dart_tasking_remote_init();
}

dart_ret_t dart_tasking_datadeps_reset()
{
  for (int i = 0; i < DART_DEPHASH_SIZE; ++i) {
    dart_dephash_elem_t *elem = local_deps[i];
    while (elem != NULL) {
      dart_dephash_elem_t *tmp = elem->next;
      dephash_recycle_elem(elem);
      elem = tmp;
    }
  }
  memset(local_deps, 0, sizeof(dart_dephash_elem_t*) * DART_DEPHASH_SIZE);
  return DART_OK;
}

dart_ret_t dart_tasking_datadeps_fini()
{
  dart_mutex_destroy(&local_deps_mutex);
  dart_mutex_destroy(&unhandled_remote_mutex);
  dart_mutex_destroy(&deferred_remote_mutex);
  dart_tasking_datadeps_reset();
  dart_dephash_elem_t *elem = freelist_head;
  while (elem != NULL) {
    dart_dephash_elem_t *tmp = elem->next;
    free(elem);
    elem = tmp;
  }
  freelist_head = NULL;
  return dart_tasking_remote_fini();
}

/**
 * Check for new remote task dependency requests coming in
 */
dart_ret_t dart_tasking_datadeps_progress()
{
  return dart_tasking_remote_progress();
}

/**
 * Allocate a new element for the dependency hash, possibly from a free-list
 */
static dart_dephash_elem_t *
dephash_allocate_elem(const dart_task_dep_t *dep, taskref task)
{
  // take an element from the free list if possible
  dart_dephash_elem_t *elem = NULL;
  if (freelist_head != NULL) {
    dart_mutex_lock(&local_deps_mutex);
    if (freelist_head != NULL) {
      DART_STACK_POP(freelist_head, elem);
    }
    dart_mutex_unlock(&local_deps_mutex);
  }

  if (elem == NULL){
    elem = calloc(1, sizeof(dart_dephash_elem_t));
  }

  DART_ASSERT(task.local != NULL);
  DART_ASSERT(elem->task.local == NULL);
  elem->task = task;
  elem->taskdep = *dep;

  return elem;
}


/**
 * Deallocate an element
 */
static void dephash_recycle_elem(dart_dephash_elem_t *elem)
{
  if (elem != NULL) {
    memset(elem, 0, sizeof(*elem));
    dart_mutex_lock(&local_deps_mutex);
    DART_STACK_PUSH(freelist_head, elem);
    dart_mutex_unlock(&local_deps_mutex);
  }
}

/**
 * Add a task with dependency to the local dependency hash table.
 */
static dart_ret_t dephash_add_local(const dart_task_dep_t *dep, taskref task)
{
  dart_dephash_elem_t *elem = dephash_allocate_elem(dep, task);
  // we can take the task's phase only for local tasks
  // so we have to do it here instead of dephash_allocate_elem
  elem->phase = task.local->phase;
  // put the new entry at the beginning of the list
  int slot = hash_gptr(dep->gptr);
  dart_mutex_lock(&local_deps_mutex);
  DART_STACK_PUSH(local_deps[slot], elem);
  dart_mutex_unlock(&local_deps_mutex);

  return DART_OK;
}

static void
release_deferred_remote_releases()
{
  dart_mutex_lock(&deferred_remote_mutex);
  dart_dephash_elem_t *elem;
  dart_dephash_elem_t *next = deferred_remote_releases;
  while ((elem = next) != NULL) {
    next = elem->next;
    dart_task_t *task = elem->task.local;
    int unresolved_deps = DART_DEC_AND_FETCH32(&task->unresolved_deps);
    DART_LOG_DEBUG("release_defered : Task with remote dep %p has %i "
                   "unresolved dependencies left", task, unresolved_deps);
    if (unresolved_deps < 0) {
      DART_LOG_ERROR("ERROR: task %p with remote dependency does not seem to "
                     "have unresolved dependencies!", task);
    } else if (unresolved_deps == 0) {
      // enqueue as runnable
      dart__tasking__enqueue_runnable(task);
    }
    dephash_recycle_elem(elem);
  }
  deferred_remote_releases = NULL;
  dart_mutex_unlock(&deferred_remote_mutex);
}

static bool
is_local_successor(dart_task_t *task, dart_task_t *candidate)
{
  for (task_list_t *elem = task->successor;
       elem != NULL;
       elem = elem->next) {
    if (elem->task == candidate) {
      return true;
    }
  }
  return false;
}

/**
 * Look at direct successors of \c task and send direct task dependency
 * requests if necessary.
 */
#if 0
static dart_ret_t
find_remote_direct_dependencies(dart_task_t *task, dart_dephash_elem_t *rdep)
{

  int slot = hash_gptr(rdep->taskdep.gptr);
  for (dart_dephash_elem_t *local = local_deps[slot];
                            local != NULL;
                            local = local->next) {
    if (IS_OUT_DEP(local->taskdep)                &&
        local->taskdep.gptr.addr_or_offs.addr
          == rdep->taskdep.gptr.addr_or_offs.addr &&
        local->phase >= rdep->phase               &&
        is_local_successor(task, local->task.local)) {
      dart_global_unit_t target = DART_GLOBAL_UNIT_ID(
                                        rdep->taskdep.gptr.unitid);
      dart_tasking_remote_direct_taskdep(
          target,
          local->task.local,
          rdep->task);
      int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                  &local->task.local->unresolved_deps);
      DART_LOG_DEBUG("DIRECT task dep: task %p depends on remote task %p at "
                     "unit %i and has %i dependencies",
                     local->task.local,
                     rdep->task,
                     target.id,
                     unresolved_deps);
    }
  }

  return DART_OK;
}
#endif

dart_ret_t
dart_tasking_datadeps_release_unhandled_remote()
{
  dart_dephash_elem_t *rdep;
  DART_LOG_DEBUG("Handling previously unhandled remote dependencies: %p",
                 unhandled_remote_deps);
  dart_mutex_lock(&unhandled_remote_mutex);
  dart_dephash_elem_t *next = unhandled_remote_deps;
  while ((rdep = next) != NULL) {
    next = rdep->next;
    /**
     * Iterate over all possible tasks and find the closest-matching
     * local task that satisfies the remote dependency.
     * For tasks with a higher phase than the resolving task, send direct
     * task dependencies.
     */
    dart_global_unit_t origin = DART_GLOBAL_UNIT_ID(rdep->taskdep.gptr.unitid);

    dart_task_t *candidate            = NULL;
    dart_task_t *direct_dep_candidate = NULL;
    DART_LOG_DEBUG("Handling delayed remote dependency for task %p from unit %i",
                   rdep->task, origin.id);
    int slot = hash_gptr(rdep->taskdep.gptr);
    for (dart_dephash_elem_t *local = local_deps[slot];
                              local != NULL;
                              local = local->next) {
      dart_task_t *task = local->task.local;
      // lock the task to avoid race condiditions in updating the state
      dart_mutex_lock(&task->mutex);
      if (local->taskdep.gptr.addr_or_offs.addr
              == rdep->taskdep.gptr.addr_or_offs.addr &&
          IS_OUT_DEP(local->taskdep) &&
          IS_ACTIVE_TASK(task)) {
        /*
         * Remote INPUT task dependencies are considered to refer to the
         * previous phase so every task in the same phase and following
         * phases have to wait for the remote task to complete.
         * Note that we are only accounting for the candidate task in the
         * lowest phase since all later tasks are handled through local
         * dependencies.
         *
         * TODO: formulate the relation of local and remote dependencies
         *       between tasks and phases!
         */
        if (task->phase >= rdep->phase) {
          dart_mutex_unlock(&task->mutex);
          if (direct_dep_candidate == NULL ||
              direct_dep_candidate->phase > task->phase) {
            direct_dep_candidate = task;
            DART_LOG_TRACE("Making local task %p a direct dependecy candidate "
                           "for remote task %p",
                           direct_dep_candidate,
                           rdep->task.remote);
          }
        } else {
          // check whether a previously encountered candidate
          // comes from an earlier phase than this candidate
          if (candidate == NULL || task->phase > candidate->phase) {
            // release the lock on the previous candidate
            if (candidate != NULL) {
              dart_mutex_unlock(&candidate->mutex);
            }
            // keep the current task/candidate locked until we find another
            // candidate or have added the remote_successor (below).
            candidate = task;
            DART_LOG_TRACE("Making local task %p a candidate for "
                           "remote task %p", candidate, rdep->task.remote);
          } else {
            dart_mutex_unlock(&task->mutex);
          }
        }
      } else {
        dart_mutex_unlock(&task->mutex);
      }
    }

    if (direct_dep_candidate != NULL) {
      // this task has to wait for the remote task to finish because it will
      // overwrite the input of the remote task

      dart_global_unit_t target = DART_GLOBAL_UNIT_ID(
                                        rdep->taskdep.gptr.unitid);
      dart_tasking_remote_direct_taskdep(
          target,
          direct_dep_candidate,
          rdep->task);
      int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                  &direct_dep_candidate->unresolved_deps);
      DART_LOG_DEBUG("DIRECT task dep: task %p (ph:%i) directly depends on "
                     "remote task %p (ph:%i) at unit %i and has %i dependencies",
                     direct_dep_candidate,
                     direct_dep_candidate->phase,
                     rdep->task,
                     rdep->phase,
                     target.id,
                     unresolved_deps);
    }

    if (candidate != NULL) {
      // we have a local task to satisfy the remote task
//      find_remote_direct_dependencies(candidate, rdep);
      DART_LOG_DEBUG("Found local task %p to satisfy remote dependency of "
                     "task %p from origin %i",
                     candidate, rdep->task.remote, origin.id);
      DART_STACK_PUSH(candidate->remote_successor, rdep);
      dart_mutex_unlock(&(candidate->mutex));
    } else {
      // the remote dependency cannot be served --> send release
      DART_LOG_DEBUG("Releasing remote task %p from unit %i, "
                     "which could not be handled in phase %i",
                     rdep->task.remote, origin.id,
                     rdep->phase);
      dart_tasking_remote_release(origin, rdep->task, &rdep->taskdep);
      dephash_recycle_elem(rdep);
    }
  }

  unhandled_remote_deps = NULL;
  dart_mutex_unlock(&unhandled_remote_mutex);

  /**
   * Finally release all defered remote dependency releases
   */
  release_deferred_remote_releases();

  return DART_OK;
}
/**
 * Find all tasks this task depends on and add the task to the dependency hash
 * table. All latest tasks are considered up to the first task with OUT|INOUT
 * dependency.
 */
dart_ret_t dart_tasking_datadeps_handle_task(
    dart_task_t           *task,
    const dart_task_dep_t *deps,
    size_t                 ndeps)
{
  dart_global_unit_t myid;
  dart_myid(&myid);
  DART_LOG_DEBUG("Datadeps: task %p has %zu data dependencies in phase %i",
                 task, ndeps, task->phase);
  for (size_t i = 0; i < ndeps; i++) {
    dart_task_dep_t dep = deps[i];
    if (dep.type == DART_DEP_IGNORE) {
      // ignored
      continue;
    }
    int slot;
    // translate the offset to an absolute address
    if (dep.type != DART_DEP_DIRECT) {
      dart_gptr_getoffset(dep.gptr, &dep.gptr.addr_or_offs.offset);
      slot = hash_gptr(dep.gptr);
      DART_LOG_TRACE("Datadeps: task %p dependency %zu: type:%i unit:%i "
                     "seg:%i addr:%p",
                     task, i, dep.type, dep.gptr.unitid, dep.gptr.segid,
                     dep.gptr.addr_or_offs.addr);
    }

    if (dep.type == DART_DEP_DIRECT) {
      dart_task_t *deptask = dep.task;
      if (deptask != DART_TASK_NULL) {
        dart_mutex_lock(&(deptask->mutex));
        if (deptask->state != DART_TASK_FINISHED) {
          dart_tasking_tasklist_prepend(&(deptask->successor), task);
          int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                        &task->unresolved_deps);
          DART_LOG_TRACE("Making task %p a direct local successor of task %p "
                         "(successor: %p, num_deps: %i)",
                         task, deptask,
                         deptask->successor, unresolved_deps);
        }
        dart_mutex_unlock(&(deptask->mutex));
      }
    } else if (dep.gptr.unitid != myid.id) {
      if (task->parent->state == DART_TASK_ROOT) {
        dart_tasking_remote_datadep(&dep, task);
      } else {
        DART_LOG_WARN("Ignoring remote dependency in nested task!");
      }
    } else {
      /*
       * iterate over all dependent tasks until we find the first task with
       * OUT|INOUT dependency on the same pointer
       */
      for (dart_dephash_elem_t *elem = local_deps[slot];
           elem != NULL; elem = elem->next)
      {
        DART_ASSERT_MSG(
            !(elem->taskdep.gptr.addr_or_offs.addr
                == dep.gptr.addr_or_offs.addr && elem->task.local == task),
            "Task already present in dependency hashmap with same dependency!");
        DART_LOG_TRACE("Task %p local dependency on %p (s:%i) vs %p (s:%i) "
                       "of task %p",
                       task,
                       dep.gptr.addr_or_offs.addr,
                       dep.gptr.segid,
                       elem->taskdep.gptr.addr_or_offs.addr,
                       elem->taskdep.gptr.segid,
                       elem->task.local);

        if (elem->taskdep.gptr.addr_or_offs.addr
              == dep.gptr.addr_or_offs.addr) {
          dart_mutex_lock(&(elem->task.local->mutex));
          DART_LOG_TRACE("Checking task %p against task %p "
                         "(deptype: %i vs %i)",
                         elem->task.local, task, elem->taskdep.type,
                         dep.type);

          if (elem->task.local->state != DART_TASK_FINISHED &&
              (IS_OUT_DEP(dep) ||
                  (dep.type == DART_DEP_IN  && IS_OUT_DEP(elem->taskdep)))){
            // OUT dependencies have to wait for all previous dependencies
            int32_t unresolved_deps = DART_INC_AND_FETCH32(
                                          &task->unresolved_deps);
            DART_LOG_TRACE("Making task %p a local successor of task %p "
                           "(successor: %p, num_deps: %i)",
                           task, elem->task.local,
                           elem->task.local->successor, unresolved_deps);
            dart_tasking_tasklist_prepend(&(elem->task.local->successor), task);
          }
          dart_mutex_unlock(&(elem->task.local->mutex));
          if (IS_OUT_DEP(elem->taskdep)) {
            // we can stop at the first OUT|INOUT dependency
            DART_LOG_TRACE("Stopping search for dependencies for task %p at "
                           "first OUT dependency encountered from task %p!",
                           task, elem->task.local);;
            break;
          }
        }
      }

      taskref tr;
      tr.local = task;
      // add this task to the hash table
      dephash_add_local(&dep, tr);
    }
  }

  return DART_OK;
}

/**
 * Look for the latest task that satisfies \c dep of a remote task pointed
 * to by \c rtask and add it to the remote successor list.
 * Note that \c dep has to be a IN dependency.
 */
dart_ret_t dart_tasking_datadeps_handle_remote_task(
    const dart_phase_dep_t *rdep,
    const taskref           remote_task,
    dart_global_unit_t      origin)
{
  if (rdep->dep.type != DART_DEP_IN) {
    DART_LOG_ERROR("Remote dependencies with type other than DART_DEP_IN are not supported!");
    return DART_ERR_INVAL;
  }

  DART_LOG_INFO("Enqueuing remote task %p from unit %i for later resolution",
    remote_task.remote, origin.id);
  // cache this request and resolve it later
  dart_dephash_elem_t *rs = dephash_allocate_elem(&rdep->dep, remote_task);
  dart_mutex_lock(&unhandled_remote_mutex);
  rs->taskdep.gptr.unitid = origin.id;
  rs->phase = rdep->phase;
  DART_STACK_PUSH(unhandled_remote_deps, rs);
  dart_mutex_unlock(&unhandled_remote_mutex);
  return DART_OK;
}


/**
 * Handle the direct task dependency between a local task and
 * it's remote successor
 */
dart_ret_t dart_tasking_datadeps_handle_remote_direct(
    dart_task_t       *local_task,
    taskref            remote_task,
    dart_global_unit_t origin)
{
  bool enqueued = false;
  dart_task_dep_t dep;
  dep.type = DART_DEP_DIRECT;
  dep.gptr = DART_GPTR_NULL;
  dep.gptr.unitid = origin.id;
  DART_LOG_DEBUG("Remote direct task dependency for task %p: %p",
      local_task, remote_task.remote);
  if (local_task->state != DART_TASK_FINISHED) {
    dart_mutex_lock(&(local_task->mutex));
    if (local_task->state != DART_TASK_FINISHED) {
      dart_dephash_elem_t *rs = dephash_allocate_elem(&dep, remote_task);
      DART_STACK_PUSH(local_task->remote_successor, rs);
      enqueued = true;
    }
    dart_mutex_unlock(&(local_task->mutex));
  }

  if (!enqueued) {
    // local task done already --> release immediately
    dart_tasking_remote_release(origin, remote_task, &dep);
  }

  return DART_OK;
}

/**
 * Release remote and local dependencies of a local task
 */
dart_ret_t dart_tasking_datadeps_release_local_task(
    dart_task_t   *task)
{
  release_remote_dependencies(task);

  // release local successors
  task_list_t *tl = task->successor;
  while (tl != NULL) {
    task_list_t *tmp = tl->next;
    int32_t unresolved_deps = DART_DEC_AND_FETCH32(&tl->task->unresolved_deps);
    DART_LOG_DEBUG("release_local_task: task %p has %i dependencies left",
                   tl->task, unresolved_deps);

    if (unresolved_deps < 0) {
      DART_LOG_ERROR("release_local_task: task %p has negative number "
                     "of dependencies:  %i", tl->task, unresolved_deps);
    } else if (unresolved_deps == 0) {
      dart__tasking__enqueue_runnable(tl->task);
    }

    dart_tasking_tasklist_deallocate_elem(tl);

    tl = tmp;
  }

  return DART_OK;
}

dart_ret_t dart_tasking_datadeps_release_remote_dep(
  dart_task_t *local_task)
{
  // block the release of the task if it's not to be executed yet
  dart_mutex_lock(&deferred_remote_mutex);
  if (local_task->phase > dart__tasking__phase_bound()) {
    // dummy dependency
    dart_task_dep_t dep = {
        .gptr = DART_GPTR_NULL,
        .type = DART_DEP_DIRECT
    };
    taskref ref = {.local = local_task};
    dart_dephash_elem_t *dr = dephash_allocate_elem(&dep, ref);
    DART_STACK_PUSH(deferred_remote_releases, dr);
    DART_LOG_DEBUG("release_remote_dep : Defering release of task %p "
                   "with remote dep from phase %lu",
                   local_task, local_task->phase);
  } else {
    // immediately release the task
    int unresolved_deps = DART_DEC_AND_FETCH32(&local_task->unresolved_deps);
    DART_LOG_DEBUG("release_remote_dep : Task %p with remote dep has %i "
                   "unresolved dependencies left", local_task, unresolved_deps);
    if (unresolved_deps < 0) {
      DART_LOG_ERROR("ERROR: task %p with remote dependency does not seem to "
                     "have unresolved dependencies!", local_task);
    } else if (unresolved_deps == 0) {
      // enqueue as runnable
      dart__tasking__enqueue_runnable(local_task);
    }
  }
  dart_mutex_unlock(&deferred_remote_mutex);
  return DART_OK;
}

dart_ret_t dart_tasking_datadeps_end_phase(uint64_t phase)
{
  // nothing to be done for now
  return DART_OK;
}


/**
 * Release the remote dependencies of \c task.
 * Also registers direct task dependencies for tasks dependent on \c task.
 */
static dart_ret_t release_remote_dependencies(dart_task_t *task)
{
  DART_LOG_TRACE("Releasing remote dependencies for task %p (rs:%p)",
                 task, task->remote_successor);
  dart_dephash_elem_t *rs = task->remote_successor;
  while (rs != NULL) {
    dart_dephash_elem_t *tmp = rs;
    rs = rs->next;

    // before sending the release we send direct task dependencies for
    // local tasks dependening on this task
//    send_direct_dependencies(tmp);

    // send the release
    dart_tasking_remote_release(
        DART_GLOBAL_UNIT_ID(tmp->taskdep.gptr.unitid),
        tmp->task,
        &tmp->taskdep);
    dephash_recycle_elem(tmp);
  }
  task->remote_successor = NULL;
  return DART_OK;
}
