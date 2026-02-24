#!/usr/bin/env bash
set -euo pipefail

# =========================
# Config (override via env)
# =========================
REPO_URL="${REPO_URL:-https://github.com/MachineLearningSystem/26FAST-PipeANN.git}"
REPO_DIR="${REPO_DIR:-$HOME/26FAST-PipeANN}"

NVME_DEV="${NVME_DEV:-/dev/nvme1n1}"
MNT="${MNT:-/mnt/nvme}"

DATA_DIR="${DATA_DIR:-$MNT/data/sift}"
INDEX_DIR="${INDEX_DIR:-$MNT/indices/sift}"
INDEX_PREFIX="${INDEX_PREFIX:-$INDEX_DIR/sift}"

# Set FORMAT_NVME=1 and I_UNDERSTAND_DATA_WILL_BE_LOST=YES to allow mkfs
FORMAT_NVME="${FORMAT_NVME:-0}"
I_UNDERSTAND_DATA_WILL_BE_LOST="${I_UNDERSTAND_DATA_WILL_BE_LOST:-NO}"

# Small test params (sift = 10K base, 100 queries)
R="${R:-64}"
L_BUILD="${L_BUILD:-96}"
B_PQ="${B_PQ:-3.3}"     # 26FAST README uses 3.3 for 100M-scale; OK for sift too :contentReference[oaicite:4]{index=4}
M_GB="${M_GB:-4}"
THREADS="${THREADS:-$(nproc)}"
METRIC="${METRIC:-l2}"

SEARCH_THREADS="${SEARCH_THREADS:-1}"
BEAM_WIDTH="${BEAM_WIDTH:-32}"
SEARCH_MODE="${SEARCH_MODE:-2}"  # 2 = PipeANN search mode in README example :contentReference[oaicite:5]{index=5}
MEM_L="${MEM_L:-0}"              # 0 = skip in-memory entry index :contentReference[oaicite:6]{index=6}
LS_LIST=(${LS_LIST:-10 20 30 40})

# Texmex sift
sift_URL="${sift_URL:-ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz}"
sift_TGZ="${sift_TGZ:-$DATA_DIR/sift.tar.gz}"

# ================
# Helper functions
# ================
log() { echo -e "\n[+] $*"; }
die() { echo -e "\n[!] ERROR: $*" >&2; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

is_mounted() { mountpoint -q "$1"; }

root_parent_dev() {
  local root_src
  root_src="$(findmnt -n -o SOURCE /)"
  # If it's like /dev/nvme0n1p1 -> parent is nvme0n1
  if [[ "$root_src" == /dev/* ]]; then
    lsblk -no PKNAME "$root_src" 2>/dev/null || true
  fi
}

nvme_parent_dev() {
  # For /dev/nvme1n1 -> parent is nvme1n1 (already a disk)
  basename "$NVME_DEV"
}

safe_mount_nvme() {
  [[ -b "$NVME_DEV" ]] || die "NVMe device not found: $NVME_DEV"

  local root_pk nvme_pk
  root_pk="$(root_parent_dev)"
  nvme_pk="$(nvme_parent_dev)"

  if [[ -n "$root_pk" && "$root_pk" == "$nvme_pk" ]]; then
    die "Refusing: $NVME_DEV appears to be the root device (/) parent."
  fi

  sudo mkdir -p "$MNT"

  if is_mounted "$MNT"; then
    log "$MNT already mounted."
    return 0
  fi

  log "Attempting to mount $NVME_DEV -> $MNT"
  if sudo mount "$NVME_DEV" "$MNT" 2>/dev/null; then
    log "Mounted successfully."
    return 0
  fi

  log "Mount failed (likely unformatted)."
  if [[ "$FORMAT_NVME" != "1" ]]; then
    die "Not formatting because FORMAT_NVME!=1. Re-run with FORMAT_NVME=1 if you intend to mkfs."
  fi
  if [[ "$I_UNDERSTAND_DATA_WILL_BE_LOST" != "YES" ]]; then
    die "Not formatting because I_UNDERSTAND_DATA_WILL_BE_LOST!=YES (safety guard)."
  fi

  log "Sanity check: existing signatures on $NVME_DEV"
  sudo wipefs -n "$NVME_DEV" || true

  log "Formatting $NVME_DEV as ext4 (DESTRUCTIVE)"
  sudo mkfs.ext4 -F "$NVME_DEV"

  log "Mounting $NVME_DEV -> $MNT"
  sudo mount "$NVME_DEV" "$MNT"
  df -h "$MNT"
}

install_deps() {
  log "Installing packages (nvme-cli + build deps)"
  sudo apt-get update -y
  sudo apt-get install -y git wget tar make cmake g++ nvme-cli \
    libaio-dev libgoogle-perftools-dev clang-format libboost-all-dev libjemalloc-dev

  # libmkl-full-dev is optional; README says MKL can be replaced by other BLAS :contentReference[oaicite:7]{index=7}
  if ! sudo apt-get install -y libmkl-full-dev; then
    log "libmkl-full-dev not available; installing OpenBLAS instead (OK per README)."
    sudo apt-get install -y libopenblas-dev
  fi

  log "NVMe devices:"
  sudo nvme list || true
}

clone_and_build() {
  log "Cloning/updating repo: $REPO_URL"
  if [[ -d "$REPO_DIR/.git" ]]; then
    (cd "$REPO_DIR" && git pull)
  else
    git clone "$REPO_URL" "$REPO_DIR"
  fi

  cd "$REPO_DIR"

  log "Building liburing (required) :contentReference[oaicite:8]{index=8}"
  (cd third_party/liburing && ./configure && make -j"$(nproc)")

  log "Building project via build.sh :contentReference[oaicite:9]{index=9}"
  bash ./build.sh
}

download_sift() {
  log "Preparing data dir: $DATA_DIR"
  sudo mkdir -p "$DATA_DIR"
  sudo chown -R "$USER":"$USER" "$DATA_DIR"

  if [[ ! -f "$sift_TGZ" ]]; then
    log "Downloading sift: $sift_URL"
    wget -c "$sift_URL" -O "$sift_TGZ"
  else
    log "Found existing $sift_TGZ"
  fi

  log "Extracting sift.tar.gz"
  tar -xzf "$sift_TGZ" -C "$DATA_DIR"

  log "Data files:"
  ls -lh "$DATA_DIR"
}

convert_to_bin() {
  cd "$REPO_DIR"

  local base_fvecs="$DATA_DIR/sift_base.fvecs"
  local query_fvecs="$DATA_DIR/sift_query.fvecs"
  local gt_ivecs="$DATA_DIR/sift_groundtruth.ivecs"

  [[ -f "$base_fvecs" ]] || die "Missing: $base_fvecs"
  [[ -f "$query_fvecs" ]] || die "Missing: $query_fvecs"
  [[ -f "$gt_ivecs" ]]   || die "Missing: $gt_ivecs"

  log "Converting fvecs/ivecs -> bin using repo utils :contentReference[oaicite:10]{index=10}"
  build/tests/utils/fvecs_to_bin "$base_fvecs"  "$DATA_DIR/sift_base.bin"
  build/tests/utils/fvecs_to_bin "$query_fvecs" "$DATA_DIR/sift_query.bin"
  build/tests/utils/ivecs_to_bin "$gt_ivecs"     "$DATA_DIR/sift_gt.bin"

  ls -lh "$DATA_DIR"/*.bin
}

build_index_and_search() {
  cd "$REPO_DIR"
  sudo mkdir -p "$INDEX_DIR"
  sudo chown -R "$USER":"$USER" "$INDEX_DIR"

  log "Building disk index (small test). Command shape matches README’s build_disk_index section :contentReference[oaicite:11]{index=11}"
  build/tests/build_disk_index float \
    "$DATA_DIR/sift_base.bin" \
    "$INDEX_PREFIX" \
    "$R" "$L_BUILD" "$B_PQ" "$M_GB" "$THREADS" "$METRIC" 0

  log "Searching disk index (26FAST arg format: ... topk metric search_mode mem_L Ls...) :contentReference[oaicite:12]{index=12}"
  build/tests/search_disk_index float "$INDEX_PREFIX" "$SEARCH_THREADS" "$BEAM_WIDTH" \
    "$DATA_DIR/sift_query.bin" \
    "$DATA_DIR/sift_gt.bin" \
    10 "$METRIC" "$SEARCH_MODE" "$MEM_L" "${LS_LIST[@]}"
}

# ======
# Main
# ======
need_cmd sudo
install_deps
safe_mount_nvme
clone_and_build
download_sift
convert_to_bin
build_index_and_search

log "Done. Index prefix: $INDEX_PREFIX"
