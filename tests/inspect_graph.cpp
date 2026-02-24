#include "graph_stats.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr uint64_t kMetadataSize = 4096;  // single-file index: graph starts here
constexpr size_t kMaxReasonableDegree = 10000000;
constexpr size_t kMaxReasonableNodes = 500000000;

bool parse_positive_size(const char *s, size_t *out) {
  if (!s || !out) return false;
  try {
    unsigned long v = std::stoul(s);
    *out = static_cast<size_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}
}  // namespace

int main(int argc, char **argv) {
  std::string graph_file;
  std::string index_file;
  std::string disk_index_file;
  std::string data_type_str;
  size_t adjacency_sample = 0;
  size_t max_neighbors_per_node = 20;
  size_t small_graph = 0;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--graph-file" && i + 1 < argc) {
      graph_file = argv[++i];
    } else if (arg == "--index-file" && i + 1 < argc) {
      index_file = argv[++i];
    } else if (arg == "--disk-index" && i + 1 < argc) {
      disk_index_file = argv[++i];
    } else if (arg == "--data-type" && i + 1 < argc) {
      data_type_str = argv[++i];
    } else if (arg == "--adjacency-sample" && i + 1 < argc) {
      const char *val = argv[++i];
      if (!parse_positive_size(val, &adjacency_sample)) {
        std::cerr << "Error: --adjacency-sample requires a positive number (got \"" << val << "\").\n";
        return 1;
      }
    } else if (arg == "--max-neighbors" && i + 1 < argc) {
      const char *val = argv[++i];
      if (!parse_positive_size(val, &max_neighbors_per_node)) {
        std::cerr << "Error: --max-neighbors requires a positive number (got \"" << val << "\").\n";
        return 1;
      }
    } else if (arg == "--small-graph" && i + 1 < argc) {
      const char *val = argv[++i];
      if (!parse_positive_size(val, &small_graph)) {
        std::cerr << "Error: --small-graph requires a positive number (got \"" << val << "\").\n";
        return 1;
      }
    } else if (arg == "--help" || arg == "-h") {
      std::cerr << "Usage: " << argv[0]
                << " (--graph-file <path> | --index-file <path> | --disk-index <path> --data-type <type>)\n"
                   "       [--adjacency-sample N] [--max-neighbors M] [--small-graph N]\n"
                   "  --graph-file <path>   Raw graph file (as written by save_graph at offset 0).\n"
                   "  --index-file <path>   Single-file unified index (graph at 4KB).\n"
                   "  --disk-index <path>   On-disk SSD index (*_disk.index). Requires --data-type.\n"
                   "  --data-type <type>    For --disk-index only: float, uint8, or int8.\n"
                   "  --adjacency-sample N  Print neighbor lists for first N nodes (default: 0 = off).\n"
                   "  --max-neighbors M     Cap neighbors per node in adjacency sample (default: 20).\n"
                   "  --small-graph N       Print first N nodes with out-neighbors and referenced_by (default: 0 = off).\n";
      return 0;
    }
  }

  int mode = (graph_file.empty() ? 0 : 1) + (index_file.empty() ? 0 : 2) + (disk_index_file.empty() ? 0 : 4);
  if (mode == 0) {
    std::cerr << "Error: provide one of --graph-file, --index-file, or --disk-index.\n";
    return 1;
  }
  if (mode != 1 && mode != 2 && mode != 4) {
    std::cerr << "Error: provide exactly one of --graph-file, --index-file, or --disk-index.\n";
    return 1;
  }
  if (mode == 4 && data_type_str.empty()) {
    std::cerr << "Error: --disk-index requires --data-type (float, uint8, or int8).\n";
    return 1;
  }

  pipeann::GraphStats s;
  std::string path;
  size_t offset = 0;
  bool is_single_file = false;
  bool use_disk_index = false;
  pipeann::DiskIndexDataType disk_data_type = pipeann::DiskIndexDataType::kFloat;

  if (mode == 4) {
    use_disk_index = true;
    if (data_type_str == "float") {
      disk_data_type = pipeann::DiskIndexDataType::kFloat;
    } else if (data_type_str == "uint8") {
      disk_data_type = pipeann::DiskIndexDataType::kUint8;
    } else if (data_type_str == "int8") {
      disk_data_type = pipeann::DiskIndexDataType::kInt8;
    } else {
      std::cerr << "Error: --data-type must be float, uint8, or int8 (got \"" << data_type_str << "\").\n";
      return 1;
    }
    s = pipeann::compute_graph_stats_from_disk_index(disk_index_file, disk_data_type);
    path = disk_index_file;
  } else if (mode == 1) {
    path = graph_file;
    offset = 0;
    s = pipeann::compute_graph_stats_from_file(path, offset);
  } else {
    path = index_file;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
      std::cerr << "Error: could not open " << path << "\n";
      return 1;
    }
    uint64_t meta[5];
    in.read(reinterpret_cast<char *>(meta), sizeof(meta));
    in.close();
    if (!in.good()) {
      std::cerr << "Error: could not read metadata (5 x uint64) from " << path << "\n";
      return 1;
    }
    if (meta[0] != kMetadataSize || meta[1] <= meta[0]) {
      std::cerr << "Error: file does not look like a single-file unified index (expected first 8 bytes = 4096, "
                   "next 8 bytes > 4096). Use --disk-index for *_disk.index files.\n";
      return 1;
    }
    offset = static_cast<size_t>(meta[0]);
    is_single_file = true;
    s = pipeann::compute_graph_stats_from_file(path, offset);
  }

  if (s.total_nodes == 0 && !use_disk_index && offset > 0) {
    std::cerr << "Error: failed to read graph from " << path << " at offset " << offset << "\n";
    return 1;
  }
  if (s.total_nodes == 0) {
    std::cerr << "Error: no nodes read (empty graph or read error).\n";
    return 1;
  }
  if (!use_disk_index && !is_single_file && (s.degree_max > kMaxReasonableDegree || s.total_nodes > kMaxReasonableNodes)) {
    std::cerr << "Error: file does not look like a raw graph. Use --disk-index for *_disk.index files.\n";
    return 1;
  }

  std::cout << "Graph structure summary: total_nodes=" << s.total_nodes << " active=" << s.active_nodes
            << " frozen=" << s.frozen_nodes << " total_edges=" << s.total_edges << " degree_min=" << s.degree_min
            << " degree_avg=" << s.degree_avg << " degree_max=" << s.degree_max
            << " weak_count(deg<2)=" << s.weak_count << " entry_point=" << s.entry_point << std::endl;

  if (adjacency_sample > 0) {
    std::cout << std::endl;
    if (use_disk_index) {
      pipeann::print_adjacency_sample_from_disk_index(path, disk_data_type, adjacency_sample,
                                                      max_neighbors_per_node, std::cout);
    } else {
      pipeann::print_adjacency_sample_from_file(path, offset, adjacency_sample, max_neighbors_per_node, std::cout);
    }
  }

  if (small_graph > 0) {
    std::cout << std::endl;
    if (use_disk_index) {
      pipeann::print_small_graph_from_disk_index(path, disk_data_type, small_graph,
                                                 max_neighbors_per_node, std::cout);
    } else {
      pipeann::print_small_graph_from_file(path, offset, small_graph, max_neighbors_per_node, std::cout);
    }
  }

  return 0;
}
