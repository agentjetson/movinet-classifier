#!/usr/bin/env bash
# Download official Google MoViNet checkpoints for temporal-classifier.
#
# Source: TF Model Garden (storage.googleapis.com/tf_model_garden/vision/movinet/)
# Paper:  https://arxiv.org/abs/2103.11511  (2021 — still the official MoViNet family)
#
# Default: MoViNet-A0-Stream (edge-friendly causal/streaming variant).
# Also supports A0-Base and A1-Stream/Base.
#
# NOTE ON ONNX:
#   main.cpp defaults to models/movinet_a0.onnx, but there is no official public
#   MoViNet ONNX from Google. This script fetches the canonical TF checkpoints.
#   ONNX export + Kinetics-600 → AgentJetson L2 mapping remain TODO in
#   src/temporal/movinet.cpp (run_onnx is still a placeholder). Until then the
#   binary falls back to the motion heuristic when the .onnx path is missing.
#
# Usage:
#   ./scripts/download_movinet.sh              # A0 stream (default)
#   ./scripts/download_movinet.sh --base       # A0 base (clip-based)
#   ./scripts/download_movinet.sh --a1         # A1 stream
#   ./scripts/download_movinet.sh --a1 --base  # A1 base
#   ./scripts/download_movinet.sh --all        # A0+A1, stream+base
#   ./scripts/download_movinet.sh --force      # re-download
#
# Requires: curl (or wget), tar

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODELS_DIR="${REPO_ROOT}/models"
GCS_BASE="https://storage.googleapis.com/tf_model_garden/vision/movinet"

VARIANT="a0"       # a0 | a1
MODE="stream"      # stream | base
DOWNLOAD_ALL=0
FORCE=0

usage() {
  sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
  exit 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --a0)     VARIANT="a0"; shift ;;
    --a1)     VARIANT="a1"; shift ;;
    --stream) MODE="stream"; shift ;;
    --base)   MODE="base"; shift ;;
    --all)    DOWNLOAD_ALL=1; shift ;;
    --force|-f) FORCE=1; shift ;;
    -h|--help) usage ;;
    *) echo "Unknown option: $1" >&2; usage ;;
  esac
done

mkdir -p "${MODELS_DIR}"

download_one() {
  local variant="$1"   # a0 | a1
  local mode="$2"      # stream | base
  local name="movinet_${variant}_${mode}"
  local url="${GCS_BASE}/${name}.tar.gz"
  local dest_dir="${MODELS_DIR}/${name}"
  local archive="${MODELS_DIR}/${name}.tar.gz"

  if [[ -d "${dest_dir}" && "${FORCE}" -eq 0 ]]; then
    local n
    n=$(find "${dest_dir}" -type f 2>/dev/null | wc -l | tr -d ' ')
    if [[ "${n}" -gt 0 ]]; then
      echo "✓ ${name} already present (${n} files): ${dest_dir}"
      return 0
    fi
  fi

  echo "↓ Downloading ${name} …"
  echo "  ${url}"

  local tmp="${archive}.partial"
  if command -v curl >/dev/null 2>&1; then
    curl -L --fail --progress-bar -o "${tmp}" "${url}"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --show-progress -O "${tmp}" "${url}"
  else
    echo "error: need curl or wget" >&2
    exit 1
  fi
  mv -f "${tmp}" "${archive}"

  echo "  extracting → ${dest_dir}"
  rm -rf "${dest_dir}"
  mkdir -p "${dest_dir}"
  # Archives unpack to a top-level folder matching the name
  tar -xzf "${archive}" -C "${MODELS_DIR}"
  # If tar created MODELS_DIR/movinet_a0_stream/, we're done; else move contents
  if [[ ! -d "${dest_dir}" ]]; then
    echo "warning: expected ${dest_dir} after extract" >&2
  fi

  local size
  size=$(wc -c < "${archive}" | tr -d ' ')
  echo "✓ ${name} ready ($(numfmt --to=iec "${size}" 2>/dev/null || echo "${size} bytes") archive)"
  echo "  checkpoint dir: ${dest_dir}"
}

if [[ "${DOWNLOAD_ALL}" -eq 1 ]]; then
  for v in a0 a1; do
    for m in stream base; do
      download_one "${v}" "${m}"
    done
  done
else
  download_one "${VARIANT}" "${MODE}"
fi

echo
echo "Done. Official checkpoints are under models/movinet_*_{stream,base}/"
echo
echo "Runtime note:"
echo "  MOVINET_MODEL still defaults to models/movinet_a0.onnx"
echo "  No official Google ONNX exists; export from the TF checkpoint (or keep"
echo "  using the motion heuristic until run_onnx() is wired)."
echo
echo "  Example layout after this script:"
echo "    models/movinet_a0_stream/   # TF checkpoint (canonical)"
echo "    models/movinet_a0.onnx      # not produced here — export TODO"
