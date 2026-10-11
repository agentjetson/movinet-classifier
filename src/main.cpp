// movinet-classifier — MoViNet temporal refine for ambiguous scenes.
// PUBLISH_MODE=ingest|nats|both for refined SceneResult

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

#include <grpcpp/grpcpp.h>
#include <google/protobuf/timestamp.pb.h>

#include <aj/edge/env.hpp>
#include <aj/edge/nats_publisher.hpp>
#include <aj/edge/otel.hpp>
#include <aj/edge/signal.hpp>

#include "capture/v1/frame.pb.h"
#include "capture/v1/frame_service.grpc.pb.h"
#include "ingest/v1/ingest_service.grpc.pb.h"
#include "scene/v1/scene.pb.h"

#include "temporal/movinet.hpp"
#include "temporal/taxonomy.hpp"

namespace {

cv::Mat decode_frame(const capture::v1::FrameEnvelope& env) {
  const std::string& enc = env.encoding();
  const std::string& payload = env.payload();
  if (enc == "jpeg" || enc == "jpg") {
    std::vector<uchar> buf(payload.begin(), payload.end());
    return cv::imdecode(buf, cv::IMREAD_COLOR);
  }
  if (enc == "raw_bgr") {
    const int w = env.width(), h = env.height();
    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (w <= 0 || h <= 0 || payload.size() < expected) return {};
    cv::Mat view(h, w, CV_8UC3, const_cast<char*>(payload.data()));
    return view.clone();
  }
  return {};
}

void fill_timestamp(google::protobuf::Timestamp* ts,
                    const google::protobuf::Timestamp& from) {
  if (from.seconds() != 0 || from.nanos() != 0) { *ts = from; return; }
  const auto now = std::chrono::system_clock::now();
  const auto secs =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
      now.time_since_epoch() - secs);
  ts->set_seconds(secs.count());
  ts->set_nanos(static_cast<int32_t>(nanos.count()));
}

scene::v1::SceneResult make_scene_result(
    const temporal::Taxonomy& tax, const temporal::TemporalDecision& d,
    int64_t frame_id, const std::string& source,
    const google::protobuf::Timestamp& frame_ts) {
  scene::v1::SceneResult sr;
  sr.set_frame_id(frame_id);
  fill_timestamp(sr.mutable_timestamp(), frame_ts);
  sr.set_source(source);
  sr.set_level1(tax.level1_for(d.level2));
  sr.set_level2(d.level2);
  sr.set_level1_confidence(d.confidence);
  sr.set_level2_confidence(d.confidence);
  for (const auto& s : tax.specialists_for(d.level2)) sr.add_specialists(s);
  sr.set_backend(d.backend.empty() ? "movinet" : d.backend);
  sr.set_temporal_requested(false);
  sr.set_notes(d.notes);
  return sr;
}

enum class PublishMode { Ingest, Nats, Both, None };
PublishMode parse_publish_mode(const std::string& s) {
  if (s == "nats") return PublishMode::Nats;
  if (s == "both" || s == "all") return PublishMode::Both;
  if (s == "none" || s == "off") return PublishMode::None;
  return PublishMode::Ingest;
}

std::string sanitize_token(std::string s) {
  for (char& c : s)
    if (c == ' ' || c == '.' || c == '/' || c == '-') c = '_';
  if (s.empty()) s = "unknown";
  return s;
}

struct ScenePublisher {
  PublishMode mode{PublishMode::Ingest};
  std::unique_ptr<aj::edge::NatsPublisher> nats;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<ingest::v1::IngestService::Stub> stub;

  void connect_ingest(const std::string& addr) {
    if (addr.empty()) return;
    channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    stub = ingest::v1::IngestService::NewStub(channel);
    spdlog::info("SceneIngest → {}", addr);
  }

  void publish(const scene::v1::SceneResult& scene) {
    const bool want_nats =
        mode == PublishMode::Nats || mode == PublishMode::Both;
    const bool want_ingest =
        mode == PublishMode::Ingest || mode == PublishMode::Both;
    if (want_nats && nats) {
      std::string payload;
      if (scene.SerializeToString(&payload)) {
        nats->publish("cv.scene." + sanitize_token(scene.level1()), payload);
        nats->publish("cv.scene.result", payload);
      }
    }
    if (want_ingest && stub) {
      ingest::v1::IngestSceneRequest req;
      *req.mutable_scene() = scene;
      ingest::v1::IngestSceneResponse resp;
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(500));
      auto status = stub->IngestScene(&ctx, req, &resp);
      if (!status.ok())
        spdlog::warn("IngestScene failed: {}", status.error_message());
    }
  }
};

void print_usage(const char* argv0) {
  std::cerr << "Usage:\n  " << argv0 << " --source <path|device> [model]\n  "
            << argv0 << " --network-path <frame_addr> [model]\n"
            << "Env: TAXONOMY_PATH MOVINET_MODEL INGEST_ADDR NATS_URL "
               "PUBLISH_MODE OTEL_*\n";
}

}  // namespace

int main(int argc, char** argv) {
  aj::edge::otel::init(
      aj::edge::getenv_or("OTEL_SERVICE_NAME", "movinet-classifier"), "0.2.0");
  aj::edge::install_stop_handlers();

  if (argc < 3) {
    print_usage(argv[0]);
    aj::edge::otel::shutdown();
    return 1;
  }

  const std::string mode = argv[1];
  const std::string arg2 = argv[2];
  const std::string model =
      argc > 3 ? argv[3]
               : aj::edge::getenv_or("MOVINET_MODEL", "models/movinet_a0.onnx");
  if (mode != "--source" && mode != "--network-path") {
    print_usage(argv[0]);
    aj::edge::otel::shutdown();
    return 1;
  }

  const std::string tax_path =
      aj::edge::getenv_or("TAXONOMY_PATH", "config/taxonomy.yaml");
  temporal::Taxonomy tax = temporal::load_taxonomy(tax_path);
  if (tax.empty()) {
    spdlog::error("taxonomy empty");
    aj::edge::otel::shutdown();
    return 1;
  }

  temporal::MoViNetClassifier::Config cfg;
  cfg.model_path = model;
  cfg.device = aj::edge::getenv_or("ORT_DEVICE", "cpu");
  cfg.clip_frames = std::stoi(aj::edge::getenv_or("CLIP_FRAMES", "16"));
  cfg.input_size = 224;
  temporal::MoViNetClassifier clf(cfg);

  const int target_fps = std::stoi(aj::edge::getenv_or("CLIP_FPS", "5"));
  const auto period =
      std::chrono::milliseconds(1000 / std::max(1, target_fps));

  const std::string ingest_addr = aj::edge::getenv_or("INGEST_ADDR", "");
  const std::string nats_url = aj::edge::getenv_or("NATS_URL", "");
  std::string pub_mode_str = aj::edge::getenv_or("PUBLISH_MODE", "");
  if (pub_mode_str.empty()) {
    if (!nats_url.empty() && !ingest_addr.empty()) pub_mode_str = "both";
    else if (!nats_url.empty()) pub_mode_str = "nats";
    else if (!ingest_addr.empty()) pub_mode_str = "ingest";
    else pub_mode_str = "none";
  }

  ScenePublisher publisher;
  publisher.mode = parse_publish_mode(pub_mode_str);
  if (publisher.mode == PublishMode::Ingest || publisher.mode == PublishMode::Both)
    publisher.connect_ingest(ingest_addr.empty() ? "localhost:50052" : ingest_addr);
  if (publisher.mode == PublishMode::Nats || publisher.mode == PublishMode::Both) {
    aj::edge::NatsPublisher::Config ncfg;
    ncfg.url = nats_url.empty() ? "nats://localhost:4222" : nats_url;
    ncfg.client_name =
        aj::edge::getenv_or("NATS_CLIENT_NAME", "movinet-classifier");
    ncfg.stream = aj::edge::getenv_or("NATS_STREAM", "CV_EVENTS");
    publisher.nats = std::make_unique<aj::edge::NatsPublisher>(std::move(ncfg));
    if (!publisher.nats->connect()) {
      spdlog::error("NATS connect failed");
      if (publisher.mode == PublishMode::Nats) {
        aj::edge::otel::shutdown();
        return 1;
      }
      publisher.nats.reset();
    }
  }

  if (mode == "--network-path") {
    const std::string frame_addr =
        arg2.empty() ? aj::edge::getenv_or("FRAME_GRPC_ADDR", "localhost:50060")
                     : arg2;
    const std::string camera_id = aj::edge::getenv_or("FRAME_CAMERA_ID", "");
    const std::string preferred_enc =
        aj::edge::getenv_or("FRAME_ENCODING", "jpeg");

    spdlog::info("movinet [network-path] frame={} PUBLISH_MODE={}", frame_addr,
                 pub_mode_str);

    while (aj::edge::running()) {
      auto frame_channel =
          grpc::CreateChannel(frame_addr, grpc::InsecureChannelCredentials());
      auto frame_stub = capture::v1::FrameService::NewStub(frame_channel);
      grpc::ClientContext ctx;
      std::thread cancel_watch([&ctx] {
        while (aj::edge::running())
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ctx.TryCancel();
      });

      capture::v1::SubscribeRequest req;
      if (!camera_id.empty()) req.set_camera_id(camera_id);
      req.set_preferred_encoding(preferred_enc);
      std::unique_ptr<grpc::ClientReader<capture::v1::FrameEnvelope>> reader(
          frame_stub->Subscribe(&ctx, req));

      capture::v1::FrameEnvelope env;
      auto last_push = std::chrono::steady_clock::now() - period;
      while (aj::edge::running() && reader->Read(&env)) {
        auto now = std::chrono::steady_clock::now();
        if (now - last_push < period) continue;
        last_push = now;
        cv::Mat frame = decode_frame(env);
        if (frame.empty()) continue;
        if (auto d = clf.push(frame)) {
          auto sr = make_scene_result(tax, *d, env.frame_id(), env.source(),
                                      env.timestamp());
          spdlog::info("clip frame={} L1={} L2={} ({:.2f})", env.frame_id(),
                       sr.level1(), sr.level2(), sr.level2_confidence());
          publisher.publish(sr);
        }
      }
      auto status = reader->Finish();
      cancel_watch.join();
      if (!aj::edge::running()) break;
      spdlog::warn("FrameService ended: {} — reconnect", status.error_message());
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (publisher.nats) publisher.nats->close();
    aj::edge::otel::shutdown();
    return 0;
  }

  const std::string& source = arg2;
  cv::VideoCapture cap;
  if (source == "0" || (source.size() == 1 && std::isdigit(source[0])))
    cap.open(std::stoi(source));
  else
    cap.open(source);
  if (!cap.isOpened()) {
    spdlog::error("Failed to open source: {}", source);
    aj::edge::otel::shutdown();
    return 1;
  }

  spdlog::info("movinet [source] source={} PUBLISH_MODE={}", source,
               pub_mode_str);
  cv::Mat frame;
  int idx = 0;
  while (aj::edge::running()) {
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
      auto sr = make_scene_result(tax, *d, idx, source, empty_ts);
      spdlog::info("clip@{} L1={} L2={} ({:.2f})", idx, sr.level1(),
                   sr.level2(), sr.level2_confidence());
      publisher.publish(sr);
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    if (elapsed < period) std::this_thread::sleep_for(period - elapsed);
  }

  if (publisher.nats) publisher.nats->close();
  aj::edge::otel::shutdown();
  return 0;
}
