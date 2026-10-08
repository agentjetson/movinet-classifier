# temporal-classifier

Lightweight streaming temporal / action classifier for **AgentJetson**.

**Role:** resolve ambiguous scene decisions from [scene-router](https://github.com/agentjetson/scene-router). Single-frame SigLIP / DINO cannot reliably distinguish driving vs stopped, traffic stop vs ordinary roadside stop, or short-term motion concepts.

**Primary backend:** MoViNet-A0 / A1 (ONNX Runtime) with a motion heuristic fallback.

**Output:** refined `scene.v1.SceneResult` (`backend=movinet`, `temporal_requested=false`) via `IngestService.IngestScene` → `cv.scene.*`.

---

## Taxonomy (single source of truth)

Level-1 / Level-2 labels and specialist gating come from
**[contract/domain/taxonomy.yaml](https://github.com/agentjetson/core/blob/main/domain/taxonomy.yaml)**.

| Path | Role |
|------|------|
| `config/taxonomy.yaml` | Local copy of the contract file (keep in sync) |
| `TAXONOMY_PATH` env | Override — point at the contract file directly |
| `config/labels.yaml` | MoViNet output → taxonomy L2 id targets only |

At startup the binary loads the taxonomy and derives:

- `level1` parent for each Level-2 decision
- `specialists[]` for the refined `SceneResult` (inherited from Level-1)

There is **no** hardcoded specialist table in `main.cpp`.

---

## Architecture

```
scene-router  →  temporal_requested=true
                      ↓
              temporal-classifier (MoViNet / heuristic)
                      ↓
              refined SceneResult (L1/L2 + specialists from taxonomy)
                      ↓
              IngestScene → NATS cv.scene.*
```

---

## Environment

```bash
export TAXONOMY_PATH=../contract/domain/taxonomy.yaml   # preferred
# or rely on config/taxonomy.yaml

export ORT_DEVICE=gpu
export MOVINET_MODEL=models/movinet_a0.onnx
export CLIP_FRAMES=16
export CLIP_FPS=5
export FRAME_GRPC_ADDR=localhost:50060
export FRAME_ENCODING=jpeg
export INGEST_ADDR=localhost:50052
```

---

## Build

Requires: CMake ≥ 3.20, OpenCV, protobuf + gRPC, (optional) ONNX Runtime. yaml-cpp is fetched if missing.

```bash
export ONNXRUNTIME_ROOT=/path/to/onnxruntime
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## Run

```bash
TAXONOMY_PATH=../contract/domain/taxonomy.yaml \
  INGEST_ADDR=localhost:50052 \
  ./build/temporal_classifier --source sample.mp4

TAXONOMY_PATH=../contract/domain/taxonomy.yaml \
  INGEST_ADDR=localhost:50052 \
  ./build/temporal_classifier --network-path localhost:50060
```

| Mode | Description |
|------|-------------|
| `--source <path\|device>` | Open `VideoCapture` inside the binary |
| `--network-path <addr>` | `FrameService.Subscribe` client; publishes via `IngestScene` when `INGEST_ADDR` is set |

---

## Design principles

1. **Temporal only when needed** — when scene-router sets `temporal_requested`.
2. **Same taxonomy** as scene-router / contract — no divergent L2 or specialist tables.
3. **Edge-first** MoViNet-A0/A1 on Jetson-class hardware.
4. **Same frame path** as object-classifier (`capture.v1.FrameService`).
5. **Same scene contract** as scene-router (`scene.v1.SceneResult`).
