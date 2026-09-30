#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace temporal {

struct TemporalDecision {
  std::string level2;          // refined: driving | stopped | traffic_stop | ...
  float confidence = 0.f;
  std::string backend = "movinet";
  std::string notes;
};

// Rolling-window MoViNet-style classifier.
// Skeleton: when ONNX weights are present runs inference; otherwise uses
// a simple motion heuristic so the wiring can be tested end-to-end.
class MoViNetClassifier {
 public:
  struct Config {
    std::string model_path;
    std::string device = "cpu";
    int clip_frames = 16;
    int input_size = 224;
  };

  explicit MoViNetClassifier(const Config& cfg);
  ~MoViNetClassifier();

  // Push a frame into the rolling buffer; returns decision when buffer is full.
  std::optional<TemporalDecision> push(const cv::Mat& bgr);

  void reset();

 private:
  Config cfg_;
  std::deque<cv::Mat> buffer_;
  bool ready_ = false;
  struct Impl;
  std::unique_ptr<Impl> impl_;

  TemporalDecision heuristic_from_buffer() const;
  TemporalDecision run_onnx() const;
};

}  // namespace temporal
