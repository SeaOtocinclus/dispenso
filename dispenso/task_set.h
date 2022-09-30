/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * @file task_set.h
 * A file providing TaskSet and ConcurrentTaskSet.  These interfaces allow the user to
 * submit/schedule multiple closures and then wait on them.
 **/

#pragma once

#include <dispenso/detail/task_set_impl.h>

namespace dispenso {

/**
 * <code>TaskSet</code> is an object that allows scheduling multiple functors to a thread pool, and
 * allows to wait on that set of tasks.  <code>TaskSet</code> supplies more efficient schedule/wait
 * than <code>ConcurrentTaskSet</code>, but at the expense of only being usable from one thread at a
 * time.
 *
 * <code>TaskSet</code> is "thread-compatible".  This means that you can safely use
 * different <code>TaskSet</code> objects on different threads concurrently. Any given
 * <code>TaskSet</code> object may only be used from a single thread, so no concurrent use of that
 * object is allowed.
 **/
class TaskSet : public TaskSetBase {
 public:
  /**
   * Construct a TaskSet with the given backing pool.
   *
   * @param pool The backing pool for this TaskSet
   * @param stealingLoadMultiplier An over-load factor.  If this factor of load is reached by the
   * underlying pool, scheduled tasks may run immediately in the calling thread.
   **/
  TaskSet(ThreadPool& p, ssize_t stealingLoadMultiplier = 4)
      : TaskSetBase(p, stealingLoadMultiplier), token_(p.work_) {}

  TaskSet(TaskSet&& other) = delete;
  TaskSet& operator=(TaskSet&& other) = delete;

  /**
   * Schedule a functor for execution on the underlying pool.  If the load on the
   * underlying pool is high, immediate inline execution may occur on the current thread.
   *
   * @param f A functor matching signature <code>void()</code>.  Best performance will come from
   * passing lambdas, other concrete functors, or <code>OnceFunction</code>, but
   * <code>std::function</code> or similarly type-erased objects will also work.
   *
   * @note If <code>f</code> can throw exceptions, then <code>schedule</code> may throw if the task
   * is run inline.  Otherwise, exceptions will be caught on the running thread and best-effort
   * propagated to the <code>ConcurrentTaskSet</code>, where the first one from the set is rethrown
   * in <code>wait</code>.
   **/
  template <typename F>
  void schedule(F&& f) {
    if (outstandingTaskCount_.load(std::memory_order_relaxed) > taskSetLoadFactor_) {
      f();
    } else {
      pool_.schedule(token_, packageTask(std::forward<F>(f)));
    }
  }

  /**
   * Schedule a functor for execution on the underlying pool.
   *
   * @param f A functor matching signature <code>void()</code>.  Best performance will come from
   * passing lambdas, other concrete functors, or <code>OnceFunction</code>, but
   * <code>std::function</code> or similarly type-erased objects will also work.
   *
   * @note If <code>f</code> can throw exceptions, then exceptions will be caught on the running
   * thread and best-effort propagated to the <code>ConcurrentTaskSet</code>, where the first one
   * from the set is rethrown in <code>wait</code>.
   **/
  template <typename F>
  void schedule(F&& f, ForceQueuingTag fq) {
    pool_.schedule(token_, packageTask(std::forward<F>(f)), fq);
  }

  /**
   * Wait for all currently scheduled functors to finish execution.  If exceptions are thrown
   * during execution of the set of tasks, <code>wait</code> will propagate the first exception.
   **/
  DISPENSO_DLL_ACCESS void wait();

  /**
   * See if the currently scheduled functors can be completed while stealing and executing at most
   * <code>maxToExecute</code> of them from the pool. If not used in conjunction with wait, there
   * may be cases that <code>tryWait</code> must be called multiple times with
   * <code>maxToExecute &gt 0</code> to prevent livelock/deadlock.  If exceptions have been
   * propagated since the last call to <code>wait</code> or <code>tryWait</code>,
   * <code>tryWait</code> will propagate the first of them.
   *
   * @param maxToExecute The maximum number of tasks to proactively execute on the current thread.
   *
   * @return <code>true</code> if all currently scheduled functors have been completed prior to
   * returning, and <code>false</code> otherwise.
   **/
  DISPENSO_DLL_ACCESS bool tryWait(size_t maxToExecute);

  /**
   * Destroy the TaskSet, first waiting for all currently scheduled functors to
   * finish execution.
   **/
  ~TaskSet() {
    wait();
  }

 private:
  moodycamel::ProducerToken token_;

  template <typename Result>
  friend class detail::FutureBase;
};

/**
 * <code>ConcurrentTaskSet</code> fulfills the same API as <code>TaskSet</code> with one minor
 * difference: It may be used to schedule tasks concurrently from multiple threads (see more below).
 * It is an object that allows scheduling multiple function-like objects to a thread pool, and
 * allows to wait on that set of tasks.
 *
 * <code>ConcurrentTaskSet</code> is "thread-compatible".  This means that you can safely use
 * different <code>ConcurrentTaskSet</code> objects on different threads concurrently.
 * <code>ConcurrentTaskSet</code> also allows multiple threads to concurrently schedule against it.
 * It is an error to call wait() concurrently with schedule() on the same
 * <code>ConcurrentTaskSet</code>.
 */
class ConcurrentTaskSet : public TaskSetBase {
 public:
  /**
   * Construct a ConcurrentTaskSet with the given backing pool.
   *
   * @param pool The backing pool for this ConcurrentTaskSet
   * @param stealingLoadMultiplier An over-load factor.  If this factor of load is reached by the
   * underlying pool, scheduled tasks may run immediately in the calling thread.
   **/
  ConcurrentTaskSet(ThreadPool& pool, ssize_t stealingLoadMultiplier = 4)
      : TaskSetBase(pool, stealingLoadMultiplier) {}

  ConcurrentTaskSet(ConcurrentTaskSet&& other) = delete;
  ConcurrentTaskSet& operator=(ConcurrentTaskSet&& other) = delete;

  /**
   * Schedule a functor for execution on the underlying pool.  If the load on the
   * underlying pool is high, immediate inline execution may occur on the current thread.
   *
   * @param f A functor matching signature <code>void()</code>.  Best performance will come from
   * passing lambdas, other concrete functors, or <code>OnceFunction</code>, but
   * <code>std::function</code> or similarly type-erased objects will also work.
   *
   * @param skipRecheck A poweruser knob that says that if we don't have enough outstanding tasks to
   * immediately work steal, we should bypass the similar check in the ThreadPool.
   *
   * @note If <code>f</code> can throw exceptions, then <code>schedule</code> may throw if the task
   * is run inline.  Otherwise, exceptions will be caught on the running thread and best-effort
   * propagated to the <code>ConcurrentTaskSet</code>, where the first one from the set is rethrown
   * in <code>wait</code>.
   **/
  template <typename F>
  void schedule(F&& f, bool skipRecheck = false) {
    if (outstandingTaskCount_.load(std::memory_order_relaxed) > taskSetLoadFactor_) {
      f();
    } else if (skipRecheck) {
      pool_.schedule(packageTask(std::forward<F>(f)), ForceQueuingTag());
    } else {
      pool_.schedule(packageTask(std::forward<F>(f)));
    }
  }

  /**
   * Schedule a functor for execution on the underlying pool.
   *
   * @param f A functor matching signature <code>void()</code>.  Best performance will come from
   * passing lambdas, other concrete functors, or <code>OnceFunction</code>, but
   * <code>std::function</code> or similarly type-erased objects will also work.
   *
   * @note If <code>f</code> can throw exceptions, then exceptions will be caught on the running
   * thread and best-effort propagated to the <code>ConcurrentTaskSet</code>, where the first one
   * from the set is rethrown in <code>wait</code>.
   **/
  template <typename F>
  void schedule(F&& f, ForceQueuingTag fq) {
    pool_.schedule(packageTask(std::forward<F>(f)), fq);
  }

  /**
   * Wait for all currently scheduled functors to finish execution.  If exceptions are thrown
   * during execution of the set of tasks, <code>wait</code> will propagate the first exception.
   **/
  DISPENSO_DLL_ACCESS void wait();

  /**
   * See if the currently scheduled functors can be completed while stealing and executing at most
   * <code>maxToExecute</code> of them from the pool. If not used in conjunction with wait, there
   * may be cases that <code>tryWait</code> must be called multiple times with
   * <code>maxToExecute &gt 0</code> to prevent livelock/deadlock.  If exceptions have been
   * propagated since the last call to <code>wait</code> or <code>tryWait</code>,
   * <code>tryWait</code> will propagate the first of them.
   *
   * @param maxToExecute The maximum number of tasks to proactively execute on the current thread.
   *
   * @return <code>true</code> if all currently scheduled functors have been completed prior to
   * returning, and <code>false</code> otherwise.
   **/
  DISPENSO_DLL_ACCESS bool tryWait(size_t maxToExecute);

  /**
   * Destroy the ConcurrentTaskSet, first waiting for all currently scheduled functors to
   * finish execution.
   **/
  ~ConcurrentTaskSet() {
    wait();
  }

 private:
  bool tryExecuteNext() {
    return pool_.tryExecuteNext();
  }

  template <typename Result>
  friend class detail::FutureBase;

  friend class detail::LimitGatedScheduler;
};

} // namespace dispenso
