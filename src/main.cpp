// temporal-classifier — MoViNet (or motion heuristic) for ambiguous scenes.
// Resolves driving / stopped / traffic_stop style temporal concepts
// requested by scene-router.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include <opencv2/opencv.hpp>
#include <spdlog/spdlog.h>

#include "common/env.hpp"
#include "temporal/movinet.hpp"

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

void print_usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --source <0|video.mp4|rtsp://...> [model]\n"
      << "\n"
      << "Env: ORT_DEVICE  MOVINET_MODEL  CLIP_FRAMES  CLIP_FPS\n";
}

}  // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::info);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (argc < 3 || std::string(argv[1]) != "--source") {
    print_usage(argv[0]);
    return 1;
  }

  const std::string source = argv[2];
  const std::string model =
      argc > 3 ? argv[3]
               : edge::getenv_or("MOVINET_MODEL", "models/movinet_a0.onnx");

  temporal::MoViNetClassifier::Config cfg;
  cfg.model_path = model;
  cfg.device = edge::getenv_or("ORT_DEVICE", "cpu");
  cfg.clip_frames = std::stoi(edge::getenv_or("CLIP_FRAMES", "16"));
  cfg.input_size = 224;

  temporal::MoViNetClassifier clf(cfg);

  cv::VideoCapture cap;
  if (source == "0" || (source.size() == 1 && std::isdigit(source[0]))) {
    cap.open(std::stoi(source));
  } else {
    cap.open(source);
  }
  if (!cap.isOpened()) {
    spdlog::error("Failed to open source: {}", source);
    return 1;
  }

  // Aim for ~CLIP_FPS sampling into the rolling window
  const int target_fps = std::stoi(edge::getenv_or("CLIP_FPS", "5"));
  const auto period = std::chrono::milliseconds(1000 / std::max(1, target_fps));

  spdlog::info("temporal-classifier running (clip_frames={}, fps≈{})",
               cfg.clip_frames, target_fps);

  cv::Mat frame;
  int idx = 0;
  while (g_running) {
    auto t0 = std::chrono::steady_clock::now();
    if (!cap.read(frame) || frame.empty()) {
      if (source.find("rtsp") != std::string::npos || source == "0") {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        continue;
      }
      break;
    }
    ++idx;

    if (auto d = clf.push(frame)) {
      spdlog::info("clip@frame={}  L2={} ({:.2f})  notes={}",
                   idx, d->level2, d->confidence, d->notes);
    }

    auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed < period)
      std::this_thread::sleep_for(period - elapsed);
  }

  spdlog::info("temporal-classifier stopped");
  return 0;
}
