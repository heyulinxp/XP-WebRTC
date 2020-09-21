/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_INCLUDE_MODULE_H_
#define MODULES_INCLUDE_MODULE_H_

#include <stdint.h>

namespace webrtc {

class ProcessThread;

class Module {
 public:
  // Returns the number of milliseconds until the module wants a worker
  // thread to call Process.
  // This method is called on the same worker thread as Process will
  // be called on.
  // TODO(tommi): Almost all implementations of this function, need to know
  // the current tick count.  Consider passing it as an argument.  It could
  // also improve the accuracy of when the next callback occurs since the
  // thread that calls Process() will also have it's tick count reference
  // which might not match with what the implementations use.
  //返回模块希望工作线程调用进程之前的毫秒数。
  //此方法在将调用进程的同一工作线程上调用。
  //TODO（tommi）：这个函数的几乎所有实现都需要知道当前的计时计数。
  //考虑将其作为一个参数传递。
  //它还可以提高下一次回调发生的准确度，因为调用Process（）的线程也将有它的滴答计数引用，
  //这可能与实现所使用的不匹配。
  virtual int64_t TimeUntilNextProcess() = 0;

  // Process any pending tasks such as timeouts.
  // Called on a worker thread.
  //处理任何挂起的任务，如超时。在工作线程上调用。
  virtual void Process() = 0;

  // This method is called when the module is attached to a *running* process
  // thread or detached from one.  In the case of detaching, |process_thread|
  // will be nullptr.
  //
  // This method will be called in the following cases:
  //
  // * Non-null process_thread:
  //   * ProcessThread::RegisterModule() is called while the thread is running.
  //   * ProcessThread::Start() is called and RegisterModule has previously
  //     been called.  The thread will be started immediately after notifying
  //     all modules.
  //
  // * Null process_thread:
  //   * ProcessThread::DeRegisterModule() is called while the thread is
  //     running.
  //   * ProcessThread::Stop() was called and the thread has been stopped.
  //
  // NOTE: This method is not called from the worker thread itself, but from
  //       the thread that registers/deregisters the module or calls Start/Stop.
  //当模块附加到一个*正在运行*的进程线程或从一个线程分离时，将调用此方法。
  //在分离的情况下，|进程|将为null ptr。
  //在以下情况下将调用此方法：
  //*非空进程线程：
  //  *在线程运行时调用ProcessThread:：RegisterModule（）。
  //  *已调用ProcessThread:：Start（），并且以前已调用RegisterModule。线程将在通知所有模块后立即启动。
  //*空进程线程：
  //  *在线程运行时调用ProcessThread:：DeRegisterModule（）。
  //  *已调用ProcessThread:：Stop（），线程已停止。
  //注意：这个方法不是从工作线程本身调用的，而是从注册/注销模块或调用Start/Stop的线程调用的。
  virtual void ProcessThreadAttached(ProcessThread* process_thread) {}

 protected:
  virtual ~Module() {}
};
}  // namespace webrtc

#endif  // MODULES_INCLUDE_MODULE_H_
