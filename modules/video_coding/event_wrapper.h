/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_EVENT_WRAPPER_H_
#define MODULES_VIDEO_CODING_EVENT_WRAPPER_H_

namespace webrtc {
enum EventTypeWrapper { kEventSignaled = 1, kEventTimeout = 2 };

class EventWrapper {
 public:
  // Factory method. Constructor disabled.
  static EventWrapper* Create();

  virtual ~EventWrapper() {}

  // Releases threads who are calling Wait() and has started waiting. Please
  // note that a thread calling Wait() will not start waiting immediately.
  // assumptions to the contrary is a very common source of issues in
  // multithreaded programming.
  // Set is sticky in the sense that it will release at least one thread
  // either immediately or some time in the future.
  //释放正在调用Wait（）并已开始等待的线程。
  //请注意，调用Wait（）的线程不会立即开始等待。
  //相反的假设是多线程编程中常见的问题来源。
  //Set是粘性的，因为它将立即或在未来某个时间释放至少一个线程。
  virtual bool Set() = 0;

  // Puts the calling thread into a wait state. The thread may be released
  // by a Set() call depending on if other threads are waiting and if so on
  // timing. The thread that was released will reset the event before leaving
  // preventing more threads from being released. If multiple threads
  // are waiting for the same Set(), only one (random) thread is guaranteed to
  // be released. It is possible that multiple (random) threads are released
  // Depending on timing.
  //
  // |max_time_ms| is the maximum time to wait in milliseconds.
  //将调用线程置于等待状态。
  //该线程可以通过Set（）调用释放，具体取决于是否有其他线程在等待，如果是，则取决于计时。
  //已释放的线程将在离开之前重置事件，以阻止释放更多线程。
  //如果多个线程正在等待同一个Set（），则保证只释放一个（随机）线程。
  //有可能根据定时释放多个（随机）线程。
  //|max_time_ms |是等待的最长时间（毫秒）。
  virtual EventTypeWrapper Wait(int max_time_ms) = 0;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_EVENT_WRAPPER_H_
