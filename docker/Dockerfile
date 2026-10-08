# syntax=docker/dockerfile:1.7
# temporal-classifier — MoViNet streaming temporal / action classifier
# Base: agentjetsonai/gecko (OpenCV 5 + ORT + protobuf/gRPC)
# rf-detr is NOT used.

FROM agentjetsonai/gecko AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    GIT_TERMINAL_PROMPT=0

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# gecko exports OpenCV_DIR, ONNXRUNTIME_ROOT, ONNXRUNTIME_ROOT_DIR
RUN rm -rf build CMakeCache.txt CMakeFiles \
    && git config --global http.version HTTP/1.1 \
    && cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target temporal_classifier

FROM agentjetsonai/gecko
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/temporal_classifier /app/temporal_classifier
COPY config /app/config
COPY models /app/models

ENV LD_LIBRARY_PATH=/opt/opencv/lib:/opt/onnxruntime/lib \
    ORT_DEVICE=cpu \
    MOVINET_MODEL=/app/models/movinet_a0.onnx \
    CLIP_FRAMES=16 \
    CLIP_FPS=5 \
    NATS_URL=nats://nats:4222 \
    FRAME_GRPC_ADDR=host.docker.internal:50060 \
    FRAME_ENCODING=jpeg \
    INGEST_ADDR=host.docker.internal:50052

WORKDIR /app
# Default: network-path (override with --source for offline demos)
CMD ["/app/temporal_classifier", "--network-path", ""]
