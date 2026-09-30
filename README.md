# temporal-classifier

Lightweight streaming temporal / action classifier for **AgentJetson**.

**Role:** resolve ambiguous scene decisions from [scene-router](https://github.com/agentjetson/scene-router). Single-frame SigLIP / DINO cannot reliably distinguish:

- driving vs stopped  
- traffic stop vs ordinary roadside stop  
- short-term motion concepts  

MoViNet (and similar streaming video models) are designed for exactly this.

**Primary backend (first prototype):** MoViNet-A0 / A1 (ONNX Runtime, edge-friendly).

**Input:** short clip or rolling window of frames (typically 8–32 frames @ 2–5 FPS from the same source as scene-router).  
**Output:** refined `SceneResult` (or `TemporalResult`) that overrides / confirms Level-2 when `temporal_requested=true`.

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
refined SceneResult (L2 + confidence)
    │
    ▼
specialist selection → RF-DETR / alpr / …
```

Keeps RF-DETR completely downstream. Does not emit ObjectEnvelopes.

---

## Contract

| Direction | Message            | Subject / notes              |
|-----------|--------------------|-------------------------------|
| In        | frame window / clip| local queue or `cv.scene.temporal.request` |
| Out       | refined SceneResult| `cv.scene.temporal` / `cv.scene.result`    |

Re-uses the same hierarchical Level-1 / Level-2 taxonomy as scene-router (`config/taxonomy.yaml` aligned).

---

## Models

| Role              | Example                         | Notes                    |
|-------------------|---------------------------------|--------------------------|
| MoViNet-A0 / A1   | `models/movinet_a0.onnx`        | streaming, low latency   |
| MoViNet-A2+       | optional higher accuracy        | more FLOPs               |

Environment:

```bash
export ORT_DEVICE=gpu
export MOVINET_MODEL=models/movinet_a0.onnx
export CLIP_FRAMES=16
export CLIP_FPS=5
export NATS_URL=nats://localhost:4222
```

---

## Build

```bash
export ONNXRUNTIME_ROOT=/path/to/onnxruntime
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## Run

```bash
# Standalone clip / video
ORT_DEVICE=gpu ./build/temporal_classifier --source sample.mp4

# Rolling window from webcam
./build/temporal_classifier --source 0

# Future: subscribe to temporal_requested side-channel from scene-router
NATS_URL=nats://localhost:4222 ./build/temporal_classifier --nats
```

---

## Design principles

1. **Temporal concepts are temporal.** Do not force them into single-frame embeddings.
2. **Only run when needed.** scene-router sets `temporal_requested`; this component stays idle otherwise.
3. **Same taxonomy.** Refined Level-2 must map to the same specialist attachment table.
4. **Edge-first.** MoViNet-A0/A1 keep latency acceptable on Jetson-class hardware.
5. **Composable.** New temporal models swap under the same ONNX interface.

---

## First prototype checklist

1. scene-router emitting decisions with `temporal_requested` on ambiguous clips.
2. MoViNet ONNX under `models/`.
3. temporal-classifier produces refined Level-2 for driving / stopped / traffic_stop.
4. End-to-end: ambiguous traffic-stop clip → temporal confirms → ALPR + officer specialists gated on.

---

*This README is the local trail marker. Keep it in sync with scene-router and the AgentJetson system overview.*
