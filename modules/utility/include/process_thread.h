/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_UTILITY_INCLUDE_PROCESS_THREAD_H_
#define MODULES_UTILITY_INCLUDE_PROCESS_THREAD_H_

#include <memory>

#include "api/task_queue/queued_task.h"
#include "api/task_queue/task_queue_base.h"

namespace rtc {
class Location;
}

namespace webrtc {
class Module;

// TODO(tommi): ProcessThread probably doesn't need to be a virtual
// interface.  There exists one override besides ProcessThreadImpl,
// MockProcessThread, but when looking at how it is used, it seems
// a nullptr might suffice (or simply an actual ProcessThread instance).
//TODO（tommi）：ProcessThread可能不需要是虚拟接口。
//除了ProcessThreadImpl，MockProcessThread之外，还有一个重写，
//但是在查看如何使用它时，nullptr似乎就足够了（或者只是一个实际的ProcessThread实例）。
class ProcessThread : public TaskQueueBase {
 public:
  ~ProcessThread() override;

  static std::unique_ptr<ProcessThread> Create(const char* thread_name);

  // Starts the worker thread.  Must be called from the construction thread.
  //启动工作线程。必须从构造线程调用。
  virtual void Start() = 0;

  // Stops the worker thread.  Must be called from the construction thread.
  //停止工作线程。必须从构造线程调用。
  virtual void Stop() = 0;

  // Wakes the thread up to give a module a chance to do processing right
  // away.  This causes the worker thread to wake up and requery the specified
  // module for when it should be called back. (Typically the module should
  // return 0 from TimeUntilNextProcess on the worker thread at that point).
  // Can be called on any thread.
  //唤醒线程，让模块有机会立即进行处理。
  //这会导致工作线程唤醒并重新查询指定的模块，以确定何时应该回调它。
  //（通常，该模块应该从工作线程上的TimeUntilNextProcess返回0）。
  //可以在任何线程上调用。
  virtual void WakeUp(Module* module) = 0;

  // Adds a module that will start to receive callbacks on the worker thread.
  // Can be called from any thread.
  //添加将开始在工作线程上接收回调的模块。可以从任何线程调用。
  virtual void RegisterModule(Module* module, const rtc::Location& from) = 0;

  // Removes a previously registered module.
  // Can be called from any thread.
  //删除先前注册的模块。可以从任何线程调用。
  virtual void DeRegisterModule(Module* module) = 0;
};

}  // namespace webrtc

#endif  // MODULES_UTILITY_INCLUDE_PROCESS_THREAD_H_
