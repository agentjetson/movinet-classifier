#include "temporal/movinet.hpp"

#include <cmath>
#include <numeric>
#include <spdlog/spdlog.h>
#include <opencv2/imgproc.hpp>

#if defined(TEMPORAL_HAS_ORT)
#include <onnxruntime_cxx_api.h>
#endif

namespace temporal {

struct MoViNetClassifier::Impl {
#if defined(TEMPORAL_HAS_ORT)
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "temporal-classifier"};
  Ort::SessionOptions opts;
  std::unique_ptr<Ort::Session> session;
#endif
};

MoViNetClassifier::MoViNetClassifier(const Config& cfg)
    : cfg_(cfg), impl_(std::make_unique<Impl>()) {
#if defined(TEMPORAL_HAS_ORT)
  try {
    if (cfg_.device == "gpu") {
      OrtCUDAProviderOptions cuda_opts{};
      impl_->opts.AppendExecutionProvider_CUDA(cuda_opts);
    }
    impl_->opts.SetIntraOpNumThreads(2);
    impl_->session = std::make_unique<Ort::Session>(
        impl_->env, cfg_.model_path.c_str(), impl_->opts);
    ready_ = true;
    spdlog::info("MoViNetClassifier ready: {}", cfg_.model_path);
  } catch (const std::exception& e) {
    spdlog::warn("MoViNet load failed ({}): {} — using motion heuristic",
                 cfg_.model_path, e.what());
    ready_ = false;
  }
#else
  spdlog::warn("ONNX Runtime not linked — temporal-classifier uses motion heuristic");
  ready_ = false;
#endif
}

MoViNetClassifier::~MoViNetClassifier() = default;

void MoViNetClassifier::reset() { buffer_.clear(); }

std::optional<TemporalDecision> MoViNetClassifier::push(const cv::Mat& bgr) {
  if (bgr.empty()) return std::nullopt;
  cv::Mat small;
  cv::resize(bgr, small, {cfg_.input_size, cfg_.input_size});
  buffer_.push_back(small.clone());
  if (static_cast<int>(buffer_.size()) > cfg_.clip_frames)
    buffer_.pop_front();
  if (static_cast<int>(buffer_.size()) < cfg_.clip_frames)
    return std::nullopt;

  if (ready_) return run_onnx();
  return heuristic_from_buffer();
}

TemporalDecision MoViNetClassifier::heuristic_from_buffer() const {
  // Simple motion magnitude: mean absolute difference across consecutive frames.
  double total = 0.0;
  int pairs = 0;
  for (size_t i = 1; i < buffer_.size(); ++i) {
    cv::Mat diff;
    cv::absdiff(buffer_[i - 1], buffer_[i], diff);
    cv::Scalar m = cv::mean(diff);
    total += (m[0] + m[1] + m[2]) / 3.0;
    ++pairs;
  }
  double avg = pairs > 0 ? total / pairs : 0.0;

  TemporalDecision d;
  // Thresholds are illustrative only.
  if (avg > 12.0) {
    d.level2 = "driving";
    d.confidence = std::min(1.f, static_cast<float>(avg / 40.0));
    d.notes = "heuristic: high motion";
  } else if (avg > 3.0) {
    d.level2 = "stopped";  // residual motion (camera shake / people)
    d.confidence = 0.55f;
    d.notes = "heuristic: low residual motion";
  } else {
    d.level2 = "stopped";
    d.confidence = 0.7f;
    d.notes = "heuristic: near-static";
  }
  return d;
}

TemporalDecision MoViNetClassifier::run_onnx() const {
  // Placeholder: real MoViNet expects a specific T×C×H×W layout and label map.
  // When weights are present the session runs; mapping to AgentJetson Level-2
  // is left as a config table (driving / stopped / traffic_stop / …).
  TemporalDecision d;
  d.level2 = "driving";
  d.confidence = 0.5f;
  d.notes = "onnx path (label mapping TODO)";
  d.backend = "movinet";
  return d;
}

}  // namespace temporal
