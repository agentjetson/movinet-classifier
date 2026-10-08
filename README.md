# temporal-classifier

Lightweight streaming temporal / action classifier for **AgentJetson**.

**Role:** resolve ambiguous scene decisions from [scene-router](https://github.com/agentjetson/scene-router). Single-frame SigLIP / DINO cannot reliably distinguish:

- driving vs stopped  
- traffic stop vs ordinary roadside stop  
- short-term motion concepts  

MoViNet (and similar streaming video models) are designed for exactly this.

**Primary backend (first prototype):** MoViNet-A0 / A1 (ONNX Runtime, edge-friendly).

**Input:** short clip or rolling window of frames (typically 8–32 frames @ 2–5 FPS from the same source as scene-router).  
**Output:** refined `SceneResult` (same contract as scene-router) that overrides / confirms Level-2 when `temporal_requested=true`. Published via `IngestService.IngestScene` → `cv.scene.result`.

---

## Architecture fit

```
scene-router
    │
    │  temporal_requested == true
    ▼
temporal-classifier (MoViNet)
    │
    ▼
refined SceneResult (L2 + confidence, temporal_requested=false)
    │
    ▼
core ingest → NATS JetStream (cv.scene.result) → specialist selection
```

Keeps RF-DETR completely downstream. Does not emit ObjectEnvelopes.

---

## Contract

| Direction | Message | Subject / notes |
|-----------|---------|-----------------|
| In | frame window / clip | local queue, `--network-path` FrameService, or (future) `cv.scene.temporal.request` |
| Out | refined `scene.v1.SceneResult` | `IngestScene` → `cv.scene.result` / `cv.scene.<level1>` |

Re-uses the same hierarchical Level-1 / Level-2 taxonomy as scene-router (`config/taxonomy.yaml` aligned with `contract/domain/taxonomy.yaml`).

---

## Models

| Role | Example | Notes |
|------|---------|-------|
| MoViNet-A0 / A1 | `models/movinet_a0.onnx` | streaming, low latency |
| MoViNet-A2+ | optional higher accuracy | more FLOPs |

Environment:

```bash
export ORT_DEVICE=gpu
export MOVINET_MODEL=models/movinet_a0.onnx
export CLIP_FRAMES=16
export CLIP_FPS=5
export NATS_URL=nats://localhost:4222
# network-path:
export FRAME_GRPC_ADDR=localhost:50060
export FRAME_CAMERA_ID=          # optional filter
export FRAME_ENCODING=jpeg       # or raw_bgr
# publish refined SceneResult (optional — log-only when empty):
export INGEST_ADDR=localhost:50052
```

---

## Build

Requires: CMake ≥ 3.20, OpenCV, protobuf + gRPC, (optional) ONNX Runtime.

```bash
export ONNXRUNTIME_ROOT=/path/to/onnxruntime
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## Run

```bash
# Standalone clip / video (optional ingest)
INGEST_ADDR=localhost:50052 ORT_DEVICE=gpu \
  ./build/temporal_classifier --source sample.mp4

# Rolling window from webcam
./build/temporal_classifier --source 0

# Network path: subscribe to camera-connector FrameService (gRPC)
# and publish refined SceneResult to core ingest
INGEST_ADDR=localhost:50052 ORT_DEVICE=gpu \
  ./build/temporal_classifier --network-path localhost:50060 models/movinet_a0.onnx

# Same with env defaults
FRAME_GRPC_ADDR=localhost:50060 FRAME_ENCODING=jpeg INGEST_ADDR=localhost:50052 \
  ./build/temporal_classifier --network-path "" models/movinet_a0.onnx

# Future: subscribe to temporal_requested side-channel from scene-router
NATS_URL=nats://localhost:4222 ./build/temporal_classifier --nats
```

### Modes

| Mode | Description |
|------|-------------|
| `--source <path\|device>` | Open `VideoCapture` inside the binary (demo / offline) |
| `--network-path <frame_grpc_addr>` | Client of camera-connector `FrameService.Subscribe` (same contract as object-classifier); publishes `SceneResult` via `IngestScene` when `INGEST_ADDR` is set |

`--network-path` samples the live stream at ~`CLIP_FPS` into the MoViNet rolling window and reconnects if the stream ends.

---

## Design principles

1. **Temporal only when needed.** Run only when scene-router sets `temporal_requested`.
2. **Same taxonomy.** Refined Level-2 must map to the same specialist attachment table.
3. **Edge-first.** MoViNet-A0/A1 keep latency acceptable on Jetson-class hardware.
4. **Composable.** New temporal models swap under the same ONNX interface.
5. **Same frame path as object-classifier.** `--network-path` uses the shared `capture.v1.FrameService` contract.
6. **Same scene contract as scene-router.** Output is `scene.v1.SceneResult` with `backend=movinet` and `temporal_requested=false`.

---

## First prototype checklist

1. scene-router emitting decisions with `temporal_requested` on ambiguous clips.
2. MoViNet ONNX under `models/`.
3. temporal-classifier produces refined Level-2 for driving / stopped / traffic_stop.
4. **Network path:** FrameService subscribe → rolling clip → SceneResult → IngestScene.
5. End-to-end: ambiguous traffic-stop clip → temporal confirms → ALPR + officer specialists gated on.

---

*This README is the local trail marker. Keep it in sync with scene-router and the AgentJetson system overview.*
