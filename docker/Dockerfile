FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libopencv-dev libspdlog-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY build/temporal_classifier /app/temporal_classifier
COPY config /app/config
COPY models /app/models
ENV ORT_DEVICE=cpu
ENTRYPOINT ["/app/temporal_classifier"]
CMD ["--source", "0"]
