#include "graph_stats.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>

namespace pipeann {

namespace {

constexpr size_t kGraphHeaderSize = 24;
constexpr size_t kDiskIndexDataOffset = 4096;
constexpr size_t kSectorLen = 4096;

size_t elem_size(DiskIndexDataType t) {
  switch (t) {
    case DiskIndexDataType::kFloat: return 4;
    case DiskIndexDataType::kUint8: return 1;
    case DiskIndexDataType::kInt8: return 1;
  }
  return 4;
}

}  // namespace

GraphStats compute_graph_stats(const std::vector<std::vector<unsigned>> &graph, size_t nd,
                               size_t num_frozen_pts, unsigned ep, unsigned weak_threshold) {
  GraphStats s;
  s.total_nodes = nd + num_frozen_pts;
  s.active_nodes = nd;
  s.frozen_nodes = num_frozen_pts;
  s.entry_point = ep;

  if (s.total_nodes == 0) {
    return s;
  }

  size_t total_edges = 0;
  size_t degree_min = std::numeric_limits<size_t>::max();
  size_t degree_max = 0;
  size_t weak_count = 0;

  for (size_t i = 0; i < s.total_nodes; i++) {
    size_t deg = graph[i].size();
    total_edges += deg;
    if (deg < degree_min)
      degree_min = deg;
    if (deg > degree_max)
      degree_max = deg;
    if (deg < weak_threshold)
      weak_count++;
  }

  s.total_edges = total_edges;
  s.degree_min = (degree_min == std::numeric_limits<size_t>::max()) ? 0 : degree_min;
  s.degree_avg = static_cast<double>(total_edges) / static_cast<double>(s.total_nodes);
  s.degree_max = degree_max;
  s.weak_count = weak_count;
  return s;
}

GraphStats compute_graph_stats_from_file(const std::string &path, size_t offset) {
  GraphStats s;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return s;
  }
  in.seekg(offset, in.beg);
  if (!in.good()) {
    return s;
  }

  uint64_t expected_file_size = 0;
  uint32_t width = 0;
  uint32_t ep = 0;
  uint64_t num_frozen_pts = 0;
  in.read(reinterpret_cast<char *>(&expected_file_size), sizeof(uint64_t));
  in.read(reinterpret_cast<char *>(&width), sizeof(uint32_t));
  in.read(reinterpret_cast<char *>(&ep), sizeof(uint32_t));
  in.read(reinterpret_cast<char *>(&num_frozen_pts), sizeof(uint64_t));
  if (!in.good()) {
    return s;
  }

  s.entry_point = ep;
  s.frozen_nodes = num_frozen_pts;

  size_t bytes_read = kGraphHeaderSize;
  size_t total_edges = 0;
  size_t degree_min = std::numeric_limits<size_t>::max();
  size_t degree_max = 0;
  size_t weak_count = 0;
  const unsigned weak_threshold = 2;
  size_t nodes = 0;

  while (bytes_read != expected_file_size && in.good()) {
    uint32_t k = 0;
    in.read(reinterpret_cast<char *>(&k), sizeof(uint32_t));
    if (!in.good()) {
      break;
    }
    total_edges += k;
    if (k < degree_min)
      degree_min = k;
    if (k > degree_max)
      degree_max = k;
    if (k < weak_threshold)
      weak_count++;
    nodes++;
    in.seekg(static_cast<std::streamoff>(k * sizeof(uint32_t)), std::ios::cur);
    if (!in.good()) {
      break;
    }
    bytes_read += sizeof(uint32_t) + static_cast<size_t>(k) * sizeof(uint32_t);
  }

  s.total_nodes = nodes;
  s.active_nodes = (nodes >= num_frozen_pts) ? (nodes - num_frozen_pts) : 0;
  s.total_edges = total_edges;
  s.degree_min = (degree_min == std::numeric_limits<size_t>::max()) ? 0 : degree_min;
  s.degree_avg = (nodes > 0) ? (static_cast<double>(total_edges) / static_cast<double>(nodes)) : 0.0;
  s.degree_max = degree_max;
  s.weak_count = weak_count;
  return s;
}

void print_graph_report(const GraphStats &s, std::ostream &out) {
  out << "Graph structure summary: total_nodes=" << s.total_nodes << " active=" << s.active_nodes
      << " frozen=" << s.frozen_nodes << " total_edges=" << s.total_edges
      << " degree_min=" << s.degree_min << " degree_avg=" << s.degree_avg
      << " degree_max=" << s.degree_max << " weak_count(deg<2)=" << s.weak_count
      << " entry_point=" << s.entry_point << std::endl;
}

void print_adjacency_sample_from_file(const std::string &path, size_t offset, size_t num_nodes,
                                      size_t max_neighbors_per_node, std::ostream &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    out << "Could not open file: " << path << std::endl;
    return;
  }
  in.seekg(offset, in.beg);
  if (!in.good()) {
    out << "Seek failed" << std::endl;
    return;
  }

  uint64_t expected_file_size = 0;
  uint32_t width = 0;
  uint32_t ep = 0;
  uint64_t num_frozen_pts = 0;
  in.read(reinterpret_cast<char *>(&expected_file_size), sizeof(uint64_t));
  in.read(reinterpret_cast<char *>(&width), sizeof(uint32_t));
  in.read(reinterpret_cast<char *>(&ep), sizeof(uint32_t));
  in.read(reinterpret_cast<char *>(&num_frozen_pts), sizeof(uint64_t));
  if (!in.good()) {
    return;
  }

  out << "Adjacency sample (first " << num_nodes << " nodes, entry_point=" << ep << "):" << std::endl;
  size_t bytes_read = kGraphHeaderSize;
  size_t node_id = 0;
  while (bytes_read != expected_file_size && node_id < num_nodes && in.good()) {
    uint32_t k = 0;
    in.read(reinterpret_cast<char *>(&k), sizeof(uint32_t));
    if (!in.good()) {
      break;
    }
    size_t to_show = (max_neighbors_per_node > 0 && k > max_neighbors_per_node) ? max_neighbors_per_node : k;
    std::vector<uint32_t> nbrs(to_show);
    if (to_show > 0) {
      in.read(reinterpret_cast<char *>(nbrs.data()), static_cast<std::streamsize>(to_show * sizeof(uint32_t)));
      if (!in.good()) {
        break;
      }
    }
    if (k > to_show) {
      in.seekg(static_cast<std::streamoff>((k - to_show) * sizeof(uint32_t)), std::ios::cur);
    }

    out << "  " << node_id << ": [";
    for (size_t i = 0; i < to_show; i++) {
      if (i > 0)
        out << ", ";
      out << nbrs[i];
    }
    if (k > to_show)
      out << ", ... (" << k << " total)";
    out << "]" << std::endl;

    bytes_read += sizeof(uint32_t) + static_cast<size_t>(k) * sizeof(uint32_t);
    node_id++;
  }
}

void print_small_graph_from_file(const std::string &path, size_t offset, size_t num_nodes,
                                 size_t max_neighbors_per_node, std::ostream &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    out << "Could not open file: " << path << std::endl;
    return;
  }
  in.seekg(offset, in.beg);
  if (!in.good()) {
    out << "Seek failed" << std::endl;
    return;
  }

  uint64_t expected_file_size = 0;
  uint32_t width = 0;
  uint32_t ep = 0;
  uint64_t num_frozen_pts = 0;
  in.read(reinterpret_cast<char *>(&expected_file_size), sizeof(uint64_t));
  in.read(reinterpret_cast<char *>(&width), sizeof(uint32_t));
  in.read(reinterpret_cast<char *>(&ep), sizeof(uint32_t));
  in.read(reinterpret_cast<char *>(&num_frozen_pts), sizeof(uint64_t));
  if (!in.good()) {
    return;
  }

  std::vector<std::vector<uint32_t>> out_nbrs(num_nodes);
  std::vector<std::vector<uint32_t>> in_nbrs(num_nodes);
  size_t bytes_read = kGraphHeaderSize;
  size_t node_id = 0;
  while (bytes_read != expected_file_size && node_id < num_nodes && in.good()) {
    uint32_t k = 0;
    in.read(reinterpret_cast<char *>(&k), sizeof(uint32_t));
    if (!in.good()) {
      break;
    }
    std::vector<uint32_t> nbrs(k);
    if (k > 0) {
      in.read(reinterpret_cast<char *>(nbrs.data()), static_cast<std::streamsize>(k * sizeof(uint32_t)));
      if (!in.good()) {
        break;
      }
    }
    out_nbrs[node_id] = std::move(nbrs);
    for (uint32_t v : out_nbrs[node_id]) {
      if (v < num_nodes) {
        in_nbrs[v].push_back(static_cast<uint32_t>(node_id));
      }
    }
    bytes_read += sizeof(uint32_t) + static_cast<size_t>(out_nbrs[node_id].size()) * sizeof(uint32_t);
    node_id++;
  }
  if (node_id < num_nodes) {
    out_nbrs.resize(node_id);
    in_nbrs.resize(node_id);
    num_nodes = node_id;
  }

  out << "Small graph (first " << num_nodes << " nodes, entry_point=" << ep << "): out-neighbors and referenced_by within sample" << std::endl;
  for (size_t i = 0; i < num_nodes; i++) {
    size_t to_show = (max_neighbors_per_node > 0 && out_nbrs[i].size() > max_neighbors_per_node)
                         ? max_neighbors_per_node
                         : out_nbrs[i].size();
    out << "  " << i << ": out [";
    for (size_t j = 0; j < to_show; j++) {
      if (j > 0) out << ", ";
      out << out_nbrs[i][j];
    }
    if (out_nbrs[i].size() > to_show) {
      out << ", ... (" << out_nbrs[i].size() << " total)";
    }
    out << "]  referenced_by [";
    for (size_t j = 0; j < in_nbrs[i].size(); j++) {
      if (j > 0) out << ", ";
      out << in_nbrs[i][j];
    }
    out << "]" << std::endl;
  }
}

GraphStats compute_graph_stats_from_disk_index(const std::string &path, DiskIndexDataType data_type) {
  GraphStats s;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return s;
  }
  uint64_t disk_nnodes, disk_ndims, medoid_id_on_file, max_node_len, nnodes_per_sector;
  size_t data_offset;  // offset in file where first data sector starts (after metadata)

  // Format A: build_disk_index (aux_utils) uses save_bin<_u64> — first 8 bytes are (npts_meta, ndims_meta), then 5+ uint64s.
  int32_t meta_npts, meta_ndims;
  in.read(reinterpret_cast<char *>(&meta_npts), sizeof(int32_t));
  in.read(reinterpret_cast<char *>(&meta_ndims), sizeof(int32_t));
  if (in.good() && meta_npts >= 5) {
    in.read(reinterpret_cast<char *>(&disk_nnodes), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&disk_ndims), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&medoid_id_on_file), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&max_node_len), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&nnodes_per_sector), sizeof(uint64_t));
    data_offset = kDiskIndexDataOffset;
  } else {
    // Format B: metadata at 0 as 5 uint64s only (no save_bin header).
    in.clear();
    in.seekg(0, in.beg);
    in.read(reinterpret_cast<char *>(&disk_nnodes), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&disk_ndims), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&medoid_id_on_file), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&max_node_len), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&nnodes_per_sector), sizeof(uint64_t));
    data_offset = kDiskIndexDataOffset;
  }
  if (!in.good()) {
    return s;
  }
  size_t esz = elem_size(data_type);
  uint64_t data_dim = disk_ndims;
  // Sanity: max_node_len must fit [coords][nnbrs][nbrs] and fit in a sector.
  if (max_node_len < data_dim * esz + sizeof(uint32_t) || max_node_len > kSectorLen) {
    return s;
  }
  s.total_nodes = disk_nnodes;
  s.active_nodes = disk_nnodes;
  s.frozen_nodes = 0;
  s.entry_point = static_cast<unsigned>(medoid_id_on_file);
  if (nnodes_per_sector == 0) {
    // Large nodes: each node spans multiple sectors; read sector-by-sector by node
    s.total_edges = 0;
    s.degree_min = 0;
    s.degree_max = 0;
    s.degree_avg = 0.0;
    s.weak_count = 0;
    return s;
  }
  size_t total_edges = 0;
  size_t degree_min = std::numeric_limits<size_t>::max();
  size_t degree_max = 0;
  size_t weak_count = 0;
  const unsigned weak_threshold = 2;
  std::vector<char> sector(kSectorLen);
  uint64_t n_sectors = (disk_nnodes + nnodes_per_sector - 1) / nnodes_per_sector;
  in.seekg(static_cast<std::streamoff>(data_offset), in.beg);
  for (uint64_t sec = 0; sec < n_sectors && in.good(); sec++) {
    in.read(sector.data(), kSectorLen);
    if (!in.good()) {
      break;
    }
    for (uint64_t j = 0; j < nnodes_per_sector; j++) {
      uint64_t node_id = sec * nnodes_per_sector + j;
      if (node_id >= disk_nnodes) {
        break;
      }
      size_t offset_in_sector = j * max_node_len;
      if (offset_in_sector + data_dim * esz + sizeof(uint32_t) > kSectorLen) {
        break;
      }
      uint32_t nnbrs;
      memcpy(&nnbrs, sector.data() + offset_in_sector + data_dim * esz, sizeof(uint32_t));
      total_edges += nnbrs;
      if (nnbrs < degree_min) degree_min = nnbrs;
      if (nnbrs > degree_max) degree_max = nnbrs;
      if (nnbrs < weak_threshold) weak_count++;
    }
  }
  s.total_edges = total_edges;
  s.degree_min = (degree_min == std::numeric_limits<size_t>::max()) ? 0 : degree_min;
  s.degree_avg = (disk_nnodes > 0) ? (static_cast<double>(total_edges) / static_cast<double>(disk_nnodes)) : 0.0;
  s.degree_max = degree_max;
  s.weak_count = weak_count;
  return s;
}

void print_adjacency_sample_from_disk_index(const std::string &path, DiskIndexDataType data_type,
                                            size_t num_nodes, size_t max_neighbors_per_node, std::ostream &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    out << "Could not open file: " << path << std::endl;
    return;
  }
  uint64_t disk_nnodes, disk_ndims, medoid_id_on_file, max_node_len, nnodes_per_sector;
  size_t data_offset;
  int32_t meta_npts, meta_ndims;
  in.read(reinterpret_cast<char *>(&meta_npts), sizeof(int32_t));
  in.read(reinterpret_cast<char *>(&meta_ndims), sizeof(int32_t));
  if (in.good() && meta_npts >= 5) {
    in.read(reinterpret_cast<char *>(&disk_nnodes), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&disk_ndims), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&medoid_id_on_file), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&max_node_len), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&nnodes_per_sector), sizeof(uint64_t));
    data_offset = kDiskIndexDataOffset;
  } else {
    in.clear();
    in.seekg(0, in.beg);
    in.read(reinterpret_cast<char *>(&disk_nnodes), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&disk_ndims), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&medoid_id_on_file), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&max_node_len), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&nnodes_per_sector), sizeof(uint64_t));
    data_offset = kDiskIndexDataOffset;
  }
  if (!in.good() || nnodes_per_sector == 0) {
    return;
  }
  size_t esz = elem_size(data_type);
  uint64_t data_dim = disk_ndims;
  if (max_node_len < data_dim * esz + sizeof(uint32_t) || max_node_len > kSectorLen) {
    return;
  }
  size_t nhood_offset_in_node = data_dim * esz;
  out << "Adjacency sample (first " << num_nodes << " nodes, entry_point=" << medoid_id_on_file << "):" << std::endl;
  std::vector<char> sector(kSectorLen);
  uint64_t n_sectors = (disk_nnodes + nnodes_per_sector - 1) / nnodes_per_sector;
  in.seekg(static_cast<std::streamoff>(data_offset), in.beg);
  size_t nodes_printed = 0;
  for (uint64_t sec = 0; sec < n_sectors && nodes_printed < num_nodes && in.good(); sec++) {
    in.read(sector.data(), kSectorLen);
    if (!in.good()) {
      break;
    }
    for (uint64_t j = 0; j < nnodes_per_sector && nodes_printed < num_nodes; j++) {
      uint64_t node_id = sec * nnodes_per_sector + j;
      if (node_id >= disk_nnodes) {
        break;
      }
      size_t offset_in_sector = j * max_node_len;
      if (offset_in_sector + nhood_offset_in_node + sizeof(uint32_t) > kSectorLen) {
        break;
      }
      uint32_t nnbrs;
      memcpy(&nnbrs, sector.data() + offset_in_sector + nhood_offset_in_node, sizeof(uint32_t));
      size_t to_show = (max_neighbors_per_node > 0 && nnbrs > max_neighbors_per_node) ? max_neighbors_per_node : nnbrs;
      size_t nbr_bytes = to_show * sizeof(uint32_t);
      out << "  " << node_id << ": [";
      if (to_show > 0 && offset_in_sector + nhood_offset_in_node + sizeof(uint32_t) + nbr_bytes <= kSectorLen) {
        std::vector<uint32_t> nbrs(to_show);
        memcpy(nbrs.data(), sector.data() + offset_in_sector + nhood_offset_in_node + sizeof(uint32_t), nbr_bytes);
        for (size_t i = 0; i < to_show; i++) {
          if (i > 0) out << ", ";
          out << nbrs[i];
        }
      }
      if (nnbrs > to_show) {
        out << ", ... (" << nnbrs << " total)";
      }
      out << "]" << std::endl;
      nodes_printed++;
    }
  }
}

void print_small_graph_from_disk_index(const std::string &path, DiskIndexDataType data_type,
                                       size_t num_nodes, size_t max_neighbors_per_node, std::ostream &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    out << "Could not open file: " << path << std::endl;
    return;
  }
  uint64_t disk_nnodes, disk_ndims, medoid_id_on_file, max_node_len, nnodes_per_sector;
  size_t data_offset;
  int32_t meta_npts, meta_ndims;
  in.read(reinterpret_cast<char *>(&meta_npts), sizeof(int32_t));
  in.read(reinterpret_cast<char *>(&meta_ndims), sizeof(int32_t));
  if (in.good() && meta_npts >= 5) {
    in.read(reinterpret_cast<char *>(&disk_nnodes), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&disk_ndims), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&medoid_id_on_file), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&max_node_len), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&nnodes_per_sector), sizeof(uint64_t));
    data_offset = kDiskIndexDataOffset;
  } else {
    in.clear();
    in.seekg(0, in.beg);
    in.read(reinterpret_cast<char *>(&disk_nnodes), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&disk_ndims), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&medoid_id_on_file), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&max_node_len), sizeof(uint64_t));
    in.read(reinterpret_cast<char *>(&nnodes_per_sector), sizeof(uint64_t));
    data_offset = kDiskIndexDataOffset;
  }
  if (!in.good() || nnodes_per_sector == 0) {
    return;
  }
  size_t esz = elem_size(data_type);
  uint64_t data_dim = disk_ndims;
  if (max_node_len < data_dim * esz + sizeof(uint32_t) || max_node_len > kSectorLen) {
    return;
  }
  size_t nhood_offset_in_node = data_dim * esz;
  if (num_nodes > disk_nnodes) {
    num_nodes = static_cast<size_t>(disk_nnodes);
  }

  std::vector<std::vector<uint32_t>> out_nbrs(num_nodes);
  std::vector<std::vector<uint32_t>> in_nbrs(num_nodes);
  std::vector<char> sector(kSectorLen);
  uint64_t n_sectors = (disk_nnodes + nnodes_per_sector - 1) / nnodes_per_sector;
  in.seekg(static_cast<std::streamoff>(data_offset), in.beg);
  size_t nodes_read = 0;
  for (uint64_t sec = 0; sec < n_sectors && nodes_read < num_nodes && in.good(); sec++) {
    in.read(sector.data(), kSectorLen);
    if (!in.good()) {
      break;
    }
    for (uint64_t j = 0; j < nnodes_per_sector && nodes_read < num_nodes; j++) {
      uint64_t node_id = sec * nnodes_per_sector + j;
      if (node_id >= disk_nnodes) {
        break;
      }
      size_t offset_in_sector = j * max_node_len;
      if (offset_in_sector + nhood_offset_in_node + sizeof(uint32_t) > kSectorLen) {
        break;
      }
      uint32_t nnbrs;
      memcpy(&nnbrs, sector.data() + offset_in_sector + nhood_offset_in_node, sizeof(uint32_t));
      size_t nbr_cap = static_cast<size_t>(nnbrs);
      if (offset_in_sector + nhood_offset_in_node + sizeof(uint32_t) + nbr_cap * sizeof(uint32_t) > kSectorLen) {
        nbr_cap = (kSectorLen - (offset_in_sector + nhood_offset_in_node + sizeof(uint32_t))) / sizeof(uint32_t);
      }
      out_nbrs[nodes_read].resize(nbr_cap);
      if (nbr_cap > 0) {
        memcpy(out_nbrs[nodes_read].data(),
               sector.data() + offset_in_sector + nhood_offset_in_node + sizeof(uint32_t),
               nbr_cap * sizeof(uint32_t));
      }
      for (uint32_t v : out_nbrs[nodes_read]) {
        if (v < num_nodes) {
          in_nbrs[v].push_back(static_cast<uint32_t>(nodes_read));
        }
      }
      nodes_read++;
    }
  }
  if (nodes_read < num_nodes) {
    out_nbrs.resize(nodes_read);
    in_nbrs.resize(nodes_read);
    num_nodes = nodes_read;
  }

  out << "Small graph (first " << num_nodes << " nodes, entry_point=" << medoid_id_on_file
      << "): out-neighbors and referenced_by within sample" << std::endl;
  for (size_t i = 0; i < num_nodes; i++) {
    size_t to_show = (max_neighbors_per_node > 0 && out_nbrs[i].size() > max_neighbors_per_node)
                         ? max_neighbors_per_node
                         : out_nbrs[i].size();
    out << "  " << i << ": out [";
    for (size_t j = 0; j < to_show; j++) {
      if (j > 0) out << ", ";
      out << out_nbrs[i][j];
    }
    if (out_nbrs[i].size() > to_show) {
      out << ", ... (" << out_nbrs[i].size() << " total)";
    }
    out << "]  referenced_by [";
    for (size_t j = 0; j < in_nbrs[i].size(); j++) {
      if (j > 0) out << ", ";
      out << in_nbrs[i][j];
    }
    out << "]" << std::endl;
  }
}

}  // namespace pipeann
