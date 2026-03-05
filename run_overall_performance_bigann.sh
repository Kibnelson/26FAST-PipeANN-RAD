#!/usr/bin/env bash
# Run overall_performance only (no data/index/gt setup).
# Use the same SCALE_M and paths as setup_26fast_pipeann_bigann.sh so paths match.
#
# Usage:
#   SCALE_M=10  ./run_overall_performance_bigann.sh
#   SCALE_M=100 ./run_overall_performance_bigann.sh
set -euo pipefail

REPO_DIR="${REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" && pwd)}"
MNT="${MNT:-/mnt/nvme}"
SCALE_M="${SCALE_M:-100}"
BASE_NPTS=$((SCALE_M * 1000000))

DATA_DIR="${DATA_DIR:-$MNT/data/bigann}"
INDEX_PREFIX="${INDEX_PREFIX:-$MNT/indices_upd/bigann/${SCALE_M}m/bigann_${SCALE_M}M}"
TRUTHSET_DIR="${TRUTHSET_DIR:-$MNT/indices_upd/bigann_gnd/${SCALE_M}M_topk}"

OVERALL_L_DISK="${OVERALL_L_DISK:-128}"
OVERALL_BEAM_WIDTH="${OVERALL_BEAM_WIDTH:-4}"
OVERALL_STEP="${OVERALL_STEP:-$SCALE_M}"
OVERALL_LS=(${OVERALL_LS:-20 30})

log() { echo -e "\n[+] $*"; }
die() { echo -e "\n[!] ERROR: $*" >&2; exit 1; }

cd "$REPO_DIR"
[[ -f build/tests/overall_performance ]] || die "Build first: run setup_26fast_pipeann_bigann.sh or build the project."

data_upd="$DATA_DIR/bigann_${SCALE_M}M.bbin"
two_x=$((SCALE_M * 2))
if [[ -f "$DATA_DIR/bigann_${two_x}M.bbin" ]]; then
  data_upd="$DATA_DIR/bigann_${two_x}M.bbin"
  log "Using 2x data for insert/delete: $data_upd"
fi
[[ -f "$data_upd" ]] || die "Data not found: $data_upd"
[[ -f "$INDEX_PREFIX"_disk.index ]] || die "Index not found: $INDEX_PREFIX"
[[ -d "$TRUTHSET_DIR" ]] || die "Truthset dir not found: $TRUTHSET_DIR"

log "Running overall_performance (scale ${SCALE_M}M, step=$OVERALL_STEP)"
build/tests/overall_performance uint8 "$data_upd" "$OVERALL_L_DISK" "$INDEX_PREFIX" \
  "$DATA_DIR/bigann_query.bbin" "$TRUTHSET_DIR" 10 "$OVERALL_BEAM_WIDTH" "$OVERALL_STEP" "${OVERALL_LS[@]}"
log "Done."




# cd /home/ubuntu/26FAST-PipeANN/build
# rm -f CMakeCache.txt
# cmake ..   # no -DPIPANN_ASAN
# make -j

# cd /home/ubuntu/26FAST-PipeANN
# SCALE_M=10 ./run_overall_performance_bigann.sh

# SCALE_M=10  ./run_overall_performance_bigann.sh
# # or
# SCALE_M=100 ./run_overall_performance_bigann.sh


# SCALE_M=20 S3_BASE=s3://fuzzydedupe/disk_ann/bigann_base.bvecs S3_QUERY=s3://fuzzydedupe/disk_ann/bigann_query.bbin ./setup_26fast_pipeann_bigann.sh
