#pragma once

#include "interface/capture_request.h"
#include "interface/capture_result.h"

#include <functional>

namespace minicam {

// Callback boundary used by the fake framework/client. In Android Camera HAL
// terms this is similar to calling process_capture_result() back into framework.
using ResultCallback = std::function<void(const CaptureResult&)>;

// Minimal framework-facing camera device/session contract. The HAL submits a
// request to the driver and returns without waiting for frame completion;
// results are delivered later through ResultCallback.
class CameraDevice {
 public:
  virtual ~CameraDevice() = default;

  // Configures the capture streams for this session.
  virtual bool configure_streams() = 0;

  // Starts driver streaming after configuration.
  virtual bool start_streaming() = 0;

  // Framework-facing request entrypoint. This should validate, record in-flight
  // state, QBUF to the driver, and return quickly.
  virtual bool process_capture_request(CaptureRequest request) = 0;

  // Completes pending/in-flight work according to HAL flush semantics.
  virtual void flush() = 0;

  // Stops streaming, unregisters events, and makes future requests fail.
  virtual void close() = 0;
};

}  // namespace minicam
