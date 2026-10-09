### Recommended stack for AgentJetson on AGX Orin

AGX Orin is strong enough that you can run detection + scene router + occasional temporal together. “SOTA” here means **best accuracy–latency under concurrent load**, not Kinetics/ImageNet leaderboards alone.
For Orin AGX we want a role-by-role best deployable set, almost always ONNX → TensorRT FP16 (not raw ORT).


| Role | Model | Format / runtime | Why |
|------|--------|------------------|-----|
| **Detection (primary)** | **RF-DETR-N or RF-DETR-S** | ONNX → **TensorRT FP16** | Already in your architecture; strong COCO/RF100-VL; no NMS; C++/TRT paths exist for Orin |
| **Scene router (zero-shot)** | **SigLIP 2 Base 224** (vision; text precomputed) | ONNX → TRT FP16 for vision | Best *small* open VL encoder with real ONNX; 2–5 FPS is easy on AGX |
| **Temporal (ambiguous only)** | **MoViNet-A0/A1 Stream** (after export) or light X3D-S-class | ONNX → TRT when available | Still the practical streaming choice; A3 only if budget allows |
| **Optional pure visual alt** | DINOv3 ViT-S/16 | ONNX → TRT | Your README’s production path once labelled |

### Detection: what “SOTA on Orin” looks like

Public Orin NX numbers (AGX is faster):

| Model | ONNX FPS | TRT FPS (approx) |
|--------|----------|------------------|
| RF-DETR-nano 384 | ~21 | **~78** |
| RF-DETR-small 512 | ~14 | **~52** |
| YOLOv8n 640 | ~36 | ~89 |

On **AGX Orin**, RF-DETR-N/S in TRT FP16 is real-time with headroom for other models. YOLO11/26-n/s are still the throughput kings, but RF-DETR is the better **accuracy / open-vocab / no-NMS** fit for your stack and is already the contract.

**Practical pick:** `rf-detr-nano` default; `small` or `medium` if AGX is dedicated and you need AP.

### Scene router: SigLIP 2 Base is the right ONNX pick

- **SigLIP 2 Base 224** — not global VL SOTA, but best **ONNX-friendly small** choice for hierarchical prompts.
- NVIDIA VSS/TAO also ships **SigLIP 2** (incl. larger SO400M) with TRT vision + ORT text — validates the family on Jetson-class GPUs.
- On AGX you *could* run **So400m @ 384**, but for 2–5 FPS routing Base is enough; reserve larger only for hard scenes.

Precompute text embeddings offline so runtime is **vision only**.

### Temporal: still no clear “SOTA ONNX”

- **MoViNet-A0/A1 Stream** — best *documented* streaming edge CNN; official asset is TF, not ONNX.
- **A3** — +accuracy, ~20× FLOPs vs A0; fine on AGX alone, less ideal when RF-DETR + SigLIP share the GPU.
- Newer sub-1M RGB action nets (X3D-style) can win on NTU/Jetson Nano benchmarks, but **Kinetics zero-shot + taxonomy mapping + mature ONNX** are weaker than MoViNet’s ecosystem.

[A Short Note about Kinetics-600](https://ar5iv.labs.arxiv.org/html/1808.01340)

### Runtime rule (more important than model choice)

```
ONNX is the interchange format
TensorRT FP16 (Jetson-built engine) is the production runtime
ORT CUDA EP is for bring-up only
```

- Build engines **on the AGX** (sm_87); engines don’t port across GPU families.
- Prefer **FP16**; INT8 only with calibration on your camera distribution.
- Don’t expect full DLA for SigLIP/ViT attention graphs — GPU TRT is the path.

### Concurrent budget (order of magnitude on AGX)

| Component | Target rate | Rough TRT cost |
|-----------|-------------|----------------|
| RF-DETR-N | 15–30 FPS | few–10 ms |
| SigLIP 2 Base vision | 2–5 FPS | tens of ms |
| MoViNet-A0 (when requested) | burst | low tens of ms / clip |
| Specialists | gated | as needed |

That fits AGX comfortably if detection is TRT and scene/temporal are sparse.

### Bottom line

| Question | Answer |
|----------|--------|
| SOTA **detection** ONNX→TRT on Orin? | **RF-DETR-N/S** (your stack) or YOLO11/26-n if pure speed |
| SOTA **zero-shot scene** small ONNX? | **SigLIP 2 Base 224** (upgrade So400m only if needed) |
| SOTA **temporal** ONNX on Orin? | No single winner; **MoViNet-A0/A1** still pragmatic streaming default once exported |

## MoViNet (eg A0/A1/A2) 
is an efficient video action-recognition model designed for real-time streaming with bounded memory and low-latency inference. Unlike foundation models such as InternVideo2 and VideoMAE V2, it prioritizes lightweight, deployable inference for specific action-recognition tasks, making it well suited for resource-constrained edge environments, though generally less capable in broad video understanding.

## VideoMAE V2 
is a self-supervised video encoder that learns spatiotemporal features through dual masking and high-ratio masked reconstruction (~90%). It efficiently captures appearance cues and motion dynamics, making it well suited for embeddings, retrieval, and search without video-text supervision. However, its limited semantic and language alignment makes it less suitable for video QA and dialogue than multimodal models like InternVideo2.

## InternVideo2-CLIP-S 
InternVideo2-CLIP-S is a multimodal foundation model combining masked video modeling, contrastive learning, and next-token prediction in a three-stage training framework. Leveraging InternVL-6B for semantic guidance and VideoMAE V2 for motion awareness, it delivers strong performance across 60+ video and audio tasks, including long-context understanding, video-text retrieval, QA, and dialogue.