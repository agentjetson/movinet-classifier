// temporal-classifier — MoViNet (or motion heuristic) for ambiguous scenes.
// Resolves driving / stopped / traffic_stop style temporal concepts
// requested by scene-router.
//
// Modes:
//   1. --source <path|device>       Standalone test (opens VideoCapture itself)
//   2. --network-path <frame_addr>  Subscribe to camera-connector FrameService
//                                   (gRPC stream of FrameEnvelope)
//                                   and publish refined SceneResult via IngestScene

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/timestamp.pb.h>

#include "capture/v1/frame.pb.h"
#include "capture/v1/frame_service.grpc.pb.h"
#include "ingest/v1/ingest_service.grpc.pb.h"
#include "scene/v1/scene.pb.h"

#include "common/env.hpp"
#include "temporal/movinet.hpp"

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

// Decode FrameEnvelope payload into BGR Mat (jpeg | raw_bgr).
cv::Mat decode_frame(const capture::v1::FrameEnvelope& env) {
  const std::string& enc = env.encoding();
  const std::string& payload = env.payload();

  if (enc == "jpeg" || enc == "jpg") {
    std::vector<uchar> buf(payload.begin(), payload.end());
    return cv::imdecode(buf, cv::IMREAD_COLOR);
  }

  if (enc == "raw_bgr") {
    const int w = env.width();
    const int h = env.height();
    const size_t expected =
        static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (w <= 0 || h <= 0 || payload.size() < expected) {
      spdlog::warn("frame {}: raw_bgr size mismatch ({} vs {}x{}x3)",
                   env.frame_id(), payload.size(), w, h);
      return {};
    }
    cv::Mat view(h, w, CV_8UC3, const_cast<char*>(payload.data()));
    return view.clone();
  }

  spdlog::warn("frame {}: unsupported encoding '{}'", env.frame_id(), enc);
  return {};
}

// Minimal L2 → specialists map (aligned with contract/domain/taxonomy.yaml).
std::vector<std::string> specialists_for(const std::string& level2) {
  static const std::unordered_map<std::string, std::vector<std::string>> k = {
      {"driving", {"alpr", "speed", "vehicle_attr"}},
      {"stopped", {"alpr", "vehicle_attr"}},
      {"traffic_stop", {"alpr", "officer", "vehicle_attr"}},
      {"congestion", {"alpr", "speed"}},
      {"crash", {"alpr", "damage"}},
      {"accident", {"alpr", "damage"}},
      {"obstruction", {"alpr"}},
      {"parked", {"alpr", "vehicle_attr"}},
  };
  auto it = k.find(level2);
  if (it != k.end()) return it->second;
  return {};
}

// Default Level-1 for roadway temporal refinements.
std::string level1_for(const std::string& level2) {
  static const std::unordered_map<std::string, std::string> k = {
      {"driving", "roadway"},
      {"stopped", "roadway"},
      {"traffic_stop", "roadway"},
      {"congestion", "roadway"},
      {"crash", "roadway"},
      {"accident", "roadway"},
      {"obstruction", "roadway"},
      {"parked", "roadway"},
      {"walking", "sidewalk"},
      {"gathering", "sidewalk"},
  };
  auto it = k.find(level2);
  return it != k.end() ? it->second : "unknown";
}

void fill_timestamp(google::protobuf::Timestamp* ts,
                    const google::protobuf::Timestamp& from) {
  if (from.seconds() != 0 || from.nanos() != 0) {
    *ts = from;
    return;
  }
  const auto now = std::chrono::system_clock::now();
  const auto secs =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      now.time_since_epoch() - secs);
  ts->set_seconds(secs.count());
  ts->set_nanos(static_cast<int32_t>(nanos.count()));
}

scene::v1::SceneResult make_scene_result(
    const temporal::TemporalDecision& d, int64_t frame_id,
    const std::string& source,
    const google::protobuf::Timestamp& frame_ts) {
  scene::v1::SceneResult sr;
  sr.set_frame_id(frame_id);
  fill_timestamp(sr.mutable_timestamp(), frame_ts);
  sr.set_source(source);
  sr.set_level1(level1_for(d.level2));
  sr.set_level2(d.level2);
  sr.set_level1_confidence(d.confidence);
  sr.set_level2_confidence(d.confidence);
  for (const auto& s : specialists_for(d.level2)) {
    sr.add_specialists(s);
  }
  sr.set_backend(d.backend.empty() ? "movinet" : d.backend);
  sr.set_temporal_requested(false);  // refined — no further temporal hand-off
  sr.set_notes(d.notes);
  return sr;
}

// Thin client for core IngestService.IngestScene. Optional: no-op when addr empty.
class SceneIngestClient {
 public:
  explicit SceneIngestClient(const std::string& addr) {
    if (addr.empty()) return;
    channel_ = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    stub_ = ingest::v1::IngestService::NewStub(channel_);
    spdlog::info("SceneIngestClient → {}", addr);
  }

  bool enabled() const { return static_cast<bool>(stub_); }

  bool publish(const scene::v1::SceneResult& scene) {
    if (!stub_) return false;
    ingest::v1::IngestSceneRequest req;
    *req.mutable_scene() = scene;
    ingest::v1::IngestSceneResponse resp;
    grpc::ClientContext ctx;
    // Short deadline so a down ingest does not stall the frame loop.
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(500));
    auto status = stub_->IngestScene(&ctx, req, &resp);
    if (!status.ok()) {
      spdlog::warn("IngestScene failed: {}", status.error_message());
      return false;
    }
    if (!resp.accepted()) {
      spdlog::warn("IngestScene rejected: {}",
                   resp.has_error() ? resp.error().message() : "unknown");
      return false;
    }
    return true;
  }

 private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<ingest::v1::IngestService::Stub> stub_;
};

void print_usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --source <0|video.mp4|rtsp://...> [model]\n"
      << "  " << argv0 << " --network-path <frame_grpc_addr> [model]\n"
      << "\n"
      << "  --source         Standalone: open VideoCapture inside classifier\n"
      << "  --network-path   Subscribe to camera-connector FrameService (gRPC)\n"
      << "  frame_grpc_addr  default localhost:50060 (or FRAME_GRPC_ADDR env)\n"
      << "\n"
      << "Environment:\n"
      << "  ORT_DEVICE  MOVINET_MODEL  CLIP_FRAMES  CLIP_FPS\n"
      << "  FRAME_GRPC_ADDR  FRAME_CAMERA_ID  FRAME_ENCODING (jpeg|raw_bgr)\n"
      << "  INGEST_ADDR       core ingest gRPC (e.g. localhost:50052); empty = log only\n";
}

}  // namespace

int main(int argc, char** argv) {
  spdlog::set_level(spdlog::level::info);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (argc < 3) {
    print_usage(argv[0]);
    return 1;
  }

  const std::string mode = argv[1];
  const std::string arg2 = argv[2];  // source OR frame_grpc_addr
  const std::string model =
      argc > 3 ? argv[3]
               : edge::getenv_or("MOVINET_MODEL", "models/movinet_a0.onnx");

  if (mode != "--source" && mode != "--network-path") {
    print_usage(argv[0]);
    return 1;
  }

  temporal::MoViNetClassifier::Config cfg;
  cfg.model_path = model;
  cfg.device = edge::getenv_or("ORT_DEVICE", "cpu");
  cfg.clip_frames = std::stoi(edge::getenv_or("CLIP_FRAMES", "16"));
  cfg.input_size = 224;

  temporal::MoViNetClassifier clf(cfg);

  const int target_fps = std::stoi(edge::getenv_or("CLIP_FPS", "5"));
  const auto period =
      std::chrono::milliseconds(1000 / std::max(1, target_fps));

  SceneIngestClient ingest(edge::getenv_or("INGEST_ADDR", ""));

  // ── Mode: --network-path (gRPC FrameService client) ─────────────────────
  if (mode == "--network-path") {
    const std::string frame_addr =
        arg2.empty() ? edge::getenv_or("FRAME_GRPC_ADDR", "localhost:50060")
                     : arg2;
    const std::string camera_id = edge::getenv_or("FRAME_CAMERA_ID", "");
    const std::string preferred_enc =
        edge::getenv_or("FRAME_ENCODING", "jpeg");

    spdlog::info(
        "temporal-classifier [network-path] frame_addr={} model={} "
        "clip_frames={} fps≈{} ingest={}",
        frame_addr, model, cfg.clip_frames, target_fps,
        ingest.enabled() ? edge::getenv_or("INGEST_ADDR", "") : "(log-only)");

    // Reconnect loop so a camera-connector restart does not kill us.
    while (g_running) {
      auto frame_channel =
          grpc::CreateChannel(frame_addr, grpc::InsecureChannelCredentials());
      auto frame_stub = capture::v1::FrameService::NewStub(frame_channel);

      grpc::ClientContext ctx;
      std::thread cancel_watch([&ctx] {
        while (g_running) {
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        ctx.TryCancel();
      });

      capture::v1::SubscribeRequest req;
      if (!camera_id.empty()) req.set_camera_id(camera_id);
      req.set_preferred_encoding(preferred_enc);

      std::unique_ptr<grpc::ClientReader<capture::v1::FrameEnvelope>> reader(
          frame_stub->Subscribe(&ctx, req));

      capture::v1::FrameEnvelope env;
      int frames = 0;
      auto last_push = std::chrono::steady_clock::now() - period;

      while (g_running && reader->Read(&env)) {
        // Sample at ~CLIP_FPS into the rolling window
        auto now = std::chrono::steady_clock::now();
        if (now - last_push < period) continue;
        last_push = now;

        cv::Mat frame = decode_frame(env);
        if (frame.empty()) continue;

        ++frames;
        if (auto d = clf.push(frame)) {
          auto sr = make_scene_result(*d, env.frame_id(), env.source(),
                                      env.timestamp());
          spdlog::info(
              "clip@frame={} id={}  L1={} L2={} ({:.2f})  specialists={}  "
              "notes={}",
              frames, env.frame_id(), sr.level1(), sr.level2(),
              sr.level2_confidence(), sr.specialists_size(), d->notes);

          if (ingest.enabled()) {
            if (ingest.publish(sr)) {
              spdlog::debug("IngestScene ok frame_id={}", env.frame_id());
            }
          }
        }

        if (frames % 30 == 0) {
          spdlog::info("network-path processed {} frames (last id={})",
                       frames, env.frame_id());
        }
      }

      auto status = reader->Finish();
      cancel_watch.join();

      if (!g_running) break;
      spdlog::warn("FrameService stream ended: {} — reconnecting in 2s",
                   status.error_message());
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    spdlog::info("temporal-classifier [network-path] stopped");
    return 0;
  }

  // ── Mode: --source (standalone VideoCapture) ────────────────────────────
  const std::string& source = arg2;

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

  spdlog::info(
      "temporal-classifier [source] source={} model={} clip_frames={} fps≈{} "
      "ingest={}",
      source, model, cfg.clip_frames, target_fps,
      ingest.enabled() ? edge::getenv_or("INGEST_ADDR", "") : "(log-only)");

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
      google::protobuf::Timestamp empty_ts;
      auto sr = make_scene_result(*d, idx, source, empty_ts);
      spdlog::info("clip@frame={}  L1={} L2={} ({:.2f})  notes={}", idx,
                   sr.level1(), sr.level2(), sr.level2_confidence(), d->notes);
      if (ingest.enabled()) {
        ingest.publish(sr);
      }
    }

    auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed < period) std::this_thread::sleep_for(period - elapsed);
  }

  spdlog::info("temporal-classifier [source] stopped");
  return 0;
}
