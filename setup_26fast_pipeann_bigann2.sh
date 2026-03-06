#!/usr/bin/env bash
# Setup and run BIGANN at configurable scale (e.g. 10M or 100M) with 1M ground-truth cadence.
# Uses S3 or local base/query, downloads BIGANN ground truth, picks smallest idx_*M.ivecs that covers scale.
#
# Usage:
#   SCALE_M=100 ./setup_26fast_pipeann_bigann.sh   # 100M (default), gt_0 .. gt_100000000 at 1M cadence
#   SCALE_M=10  ./setup_26fast_pipeann_bigann.sh   # 10M,  gt_0 .. gt_10000000 at 1M cadence
#   S3_BASE=s3://bucket/bigann_base.bvecs S3_QUERY=s3://bucket/bigann_query.bbin ./setup_26fast_pipeann_bigann.sh
set -euo pipefail

REPO_DIR="${REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]:-.}")" && pwd)}"
MNT="${MNT:-/mnt/nvme}"

# Scale in millions (10 or 100). Ground truth at 1M cadence: gt_0.bin, gt_1000000.bin, ...
SCALE_M="${SCALE_M:-100}"
BASE_NPTS=$((SCALE_M * 1000000))

DATA_DIR="${DATA_DIR:-$MNT/data/bigann}"
INDEX_DIR="${INDEX_DIR:-$MNT/indices_upd/bigann/${SCALE_M}m}"
INDEX_PREFIX="${INDEX_PREFIX:-$INDEX_DIR/bigann_${SCALE_M}M}"
GND_DIR="${GND_DIR:-$MNT/data/bigann/gnd}"
TRUTHSET_DIR="${TRUTHSET_DIR:-$MNT/indices_upd/bigann_gnd/${SCALE_M}M_topk}"

S3_BASE="${S3_BASE:-}"
S3_QUERY="${S3_QUERY:-}"
BIGANN_GND_URL="${BIGANN_GND_URL:-ftp://ftp.irisa.fr/local/texmex/corpus/bigann_gnd.tar.gz}"

R="${R:-96}"
L_BUILD="${L_BUILD:-128}"
B_PQ="${B_PQ:-3.3}"
M_GB="${M_GB:-16}"
THREADS="${THREADS:-$(nproc)}"
OVERALL_L_DISK="${OVERALL_L_DISK:-128}"
OVERALL_BEAM_WIDTH="${OVERALL_BEAM_WIDTH:-4}"
OVERALL_STEP="${OVERALL_STEP:-$SCALE_M}"
OVERALL_LS=(${OVERALL_LS:-20 30})
RUN_OVERALL_PERF="${RUN_OVERALL_PERF:-1}"

GND_SIZES=(1 2 5 10 20 50 100 200 500 1000)

log() { echo -e "\n[+] $*"; }
die() { echo -e "\n[!] ERROR: $*" >&2; exit 1; }
need_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

# Run a command as the user who invoked sudo (so AWS credentials from ~/.aws or env are used)
run_as_user() {
  if [[ -n "${SUDO_USER:-}" ]]; then
    sudo -u "$SUDO_USER" "$@"
  else
    "$@"
  fi
}

pick_gnd_ivecs() {
  for m in "${GND_SIZES[@]}"; do
    (( m >= SCALE_M )) || continue
    local f="${GND_DIR}/idx_${m}M.ivecs"
    if [[ -f "$f" ]]; then echo "$f"; return 0; fi
  done
  die "No idx_*M.ivecs in $GND_DIR for scale ${SCALE_M}M (need one of: ${GND_SIZES[*]}M)"
}

build_if_needed() {
  cd "$REPO_DIR"
  if [[ ! -f build/tests/overall_performance ]]; then
    log "Building project..."
    [[ -f build.sh ]] && bash build.sh || { mkdir -p build && cd build && cmake .. && make -j; cd ..; }
  fi
  for exe in build/tests/change_pts build/tests/gt_update build/tests/build_disk_index build/tests/overall_performance; do
    [[ -f "$exe" ]] || die "Not found: $exe"
  done
  if [[ ! -f build/tests/utils/bvecs_to_bin ]] && [[ ! -f build/utils/bvecs_to_bin ]]; then
    die "Build utils (bvecs_to_bin, ivecs_to_bin) not found."
  fi
}

prepare_data() {
  log "Preparing BIGANN data (scale ${SCALE_M}M)"
  # Ensure we can write to DATA_DIR (e.g. /mnt/nvme was mounted or left root-owned)
  if [[ -n "${S3_BASE:-}" ]] || [[ -n "${S3_QUERY:-}" ]]; then
    if [[ ! -w "${MNT}" ]] || ! mkdir -p "$DATA_DIR" 2>/dev/null || [[ ! -w "$DATA_DIR" ]]; then
      log "Making $MNT writable by $USER so S3 downloads can write to $DATA_DIR (sudo may prompt)"
      sudo chown -R "$USER:$USER" "$MNT"
      mkdir -p "$DATA_DIR"
    fi
  else
    mkdir -p "$DATA_DIR"
  fi
  # If using S3 under sudo, ensure DATA_DIR is writable by the real user (for run_as_user aws)
  if [[ -n "${SUDO_USER:-}" ]] && { [[ -n "${S3_BASE:-}" ]] || [[ -n "${S3_QUERY:-}" ]]; }; then
    chown -R "$SUDO_USER" "$DATA_DIR" 2>/dev/null || true
  fi
  local base_bvecs="$DATA_DIR/bigann_base.bvecs"
  local query_bvecs="$DATA_DIR/bigann_query.bvecs"
  local base_bbin="$DATA_DIR/bigann_1B.bbin"
  local query_bbin="$DATA_DIR/bigann_query.bbin"
  if [[ -n "${S3_BASE:-}" ]]; then
    need_cmd aws
    run_as_user aws s3 cp "$S3_BASE" "$base_bvecs" || die "S3 base download failed (run without sudo if AWS credentials are only in your user env)"
  fi
  if [[ -n "${S3_QUERY:-}" ]]; then
    need_cmd aws
    run_as_user aws s3 cp "$S3_QUERY" "$query_bbin" 2>/dev/null || run_as_user aws s3 cp "$S3_QUERY" "$query_bvecs" || die "S3 query download failed"
  fi
  local utils="build/tests/utils"
  [[ -d build/utils ]] && utils="build/utils"
  if [[ -f "$base_bvecs" ]] && [[ ! -f "$base_bbin" ]]; then
    "$utils/bvecs_to_bin" "$base_bvecs" "$base_bbin"
  fi
  if [[ -f "$query_bvecs" ]] && [[ ! -f "$query_bbin" ]]; then
    "$utils/bvecs_to_bin" "$query_bvecs" "$query_bbin"
  fi
  [[ -f "$base_bbin" ]] || die "Base bin not found"
  [[ -f "$query_bbin" ]] || die "Query bin not found"
  python3 -c "
import struct
for fn in ['$base_bbin','$query_bbin']:
    with open(fn,'rb') as f: n,d=struct.unpack('<ii',f.read(8))
    print(fn,'npts=',n,'dim=',d)
"
}

create_subset() {
  log "Creating ${SCALE_M}M subset"
  local base_bbin="$DATA_DIR/bigann_1B.bbin"
  local out_bbin="$DATA_DIR/bigann_${SCALE_M}M.bbin"
  [[ -f "$base_bbin" ]] || die "Base bin not found: $base_bbin"
  if [[ ! -f "$out_bbin" ]]; then
    build/tests/change_pts uint8 "$base_bbin" "$BASE_NPTS"
    mv "${base_bbin}${BASE_NPTS}" "$out_bbin"
  fi
  # 2x subset for overall_performance (insert+delete needs extra vectors)
  if [[ $SCALE_M -lt 500 ]]; then
    local two_x=$((SCALE_M * 2))
    local out_2x="$DATA_DIR/bigann_${two_x}M.bbin"
    if [[ ! -f "$out_2x" ]]; then
      build/tests/change_pts uint8 "$base_bbin" "$((two_x * 1000000))"
      mv "${base_bbin}$((two_x * 1000000))" "$out_2x"
      log "Created 2x subset: $out_2x"
    fi
  fi
  python3 -c "import struct; f=open('$out_bbin','rb'); n,d=struct.unpack('<ii',f.read(8)); print('Subset npts=',n,'dim=',d)"
}

build_index() {
  log "Building disk index (${SCALE_M}M)"
  local data_bin="$DATA_DIR/bigann_${SCALE_M}M.bbin"
  [[ -f "$data_bin" ]] || die "Data not found: $data_bin"
  mkdir -p "$INDEX_DIR"
  build/tests/build_disk_index uint8 "$data_bin" "$INDEX_PREFIX" "$R" "$L_BUILD" "$B_PQ" "$M_GB" "$THREADS" l2 0
  ls -lh "${INDEX_PREFIX}_disk.index" 2>/dev/null || true
}

prepare_ground_truth() {
  log "Preparing ground truth (1M cadence)"
  mkdir -p "$GND_DIR" "$TRUTHSET_DIR"
  if [[ ! -f "$GND_DIR/idx_1M.ivecs" ]] && [[ ! -f "$GND_DIR/idx_200M.ivecs" ]]; then
    log "Downloading BIGANN gnd: $BIGANN_GND_URL"
    need_cmd wget
    wget -q -O "$DATA_DIR/bigann_gnd.tar.gz" "$BIGANN_GND_URL" || die "Download failed"
    tar -xzf "$DATA_DIR/bigann_gnd.tar.gz" -C "$DATA_DIR"
    [[ -d "$DATA_DIR/gnd" ]] && { mv "$DATA_DIR/gnd"/* "$GND_DIR/" 2>/dev/null; rmdir "$DATA_DIR/gnd" 2>/dev/null; }
  fi
  local idx_ivecs; idx_ivecs="$(pick_gnd_ivecs)"
  log "Using: $idx_ivecs"
  local utils="build/tests/utils"; [[ -d build/utils ]] && utils="build/utils"
  local truth_bin="$DATA_DIR/truth_${SCALE_M}M_top1000.bin"
  if [[ ! -f "$truth_bin" ]]; then
    "$utils/ivecs_to_bin" "$idx_ivecs" "$truth_bin"
  fi
  # # batch_npts must be >= index size so each range [st, st+batch_npts) contains the full index and every query has >=10 gt.
  # # step=1M gives gt_0.bin, gt_1000000.bin, ... for overall_performance.
  # local tot_npts=$((BASE_NPTS + 1000000))
  # build/tests/gt_update "$truth_bin" "$tot_npts" "$BASE_NPTS" 10 "$TRUTHSET_DIR" 0 1000000
  # log "Truthset: $(ls "$TRUTHSET_DIR"/gt_*.bin 2>/dev/null | wc -l) files"
  # ls -lh "$TRUTHSET_DIR/gt_0.bin" "$TRUTHSET_DIR/gt_1000000.bin" 2>/dev/null || true


  # batch_npts must be >= index size so each range [st, st+batch_npts) contains the full index and every query has >=10 gt.
  # step=100K gives gt_0.bin, gt_100000.bin, ... gt_900000.bin (10 checkpoints over 1M cadence) for overall_performance.
  local tot_npts=$((BASE_NPTS + 1000000))
  build/tests/gt_update "$truth_bin" "$tot_npts" "$BASE_NPTS" 10 "$TRUTHSET_DIR" 0 100000
  log "Truthset: $(ls "$TRUTHSET_DIR"/gt_*.bin 2>/dev/null | wc -l) files"
  ls -lh "$TRUTHSET_DIR/gt_0.bin" "$TRUTHSET_DIR/gt_900000.bin" 2>/dev/null || true


}

run_overall_performance() {
  cd "$REPO_DIR"
  log "Running overall_performance (scale ${SCALE_M}M, step=$OVERALL_STEP)"
  local data_upd="$DATA_DIR/bigann_${SCALE_M}M.bbin"
  local two_x=$((SCALE_M * 2))
  if [[ -f "$DATA_DIR/bigann_${two_x}M.bbin" ]]; then
    data_upd="$DATA_DIR/bigann_${two_x}M.bbin"
    log "Using 2x data for insert/delete: $data_upd"
  fi
  build/tests/overall_performance uint8 "$data_upd" "$OVERALL_L_DISK" "$INDEX_PREFIX" \
    "$DATA_DIR/bigann_query.bbin" "$TRUTHSET_DIR" 10 "$OVERALL_BEAM_WIDTH" "$OVERALL_STEP" "${OVERALL_LS[@]}"
}

main() {
  # need_cmd python3
  # build_if_needed
  # prepare_data
  # create_subset
  # build_index
  prepare_ground_truth
  if [[ "${RUN_OVERALL_PERF:-1}" == "1" ]]; then run_overall_performance; fi
  log "Done. Scale=${SCALE_M}M Index=$INDEX_PREFIX Truthset=$TRUTHSET_DIR"
  log "To run overall_performance again without re-running setup: SCALE_M=$SCALE_M ./run_overall_performance_bigann.sh"
  log "Optional: for insert-only test_insert_search, run gt_update with insert_only=1 into another dir and use that truthset."
}
main "$@"
