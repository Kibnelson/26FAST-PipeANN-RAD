#pragma once

#include <cstddef>
#include <iostream>
#include <ostream>
#include <vector>

namespace pipeann {

struct GraphStats {
  size_t total_nodes = 0;
  size_t active_nodes = 0;
  size_t frozen_nodes = 0;
  size_t total_edges = 0;
  size_t degree_min = 0;
  double degree_avg = 0.0;
  size_t degree_max = 0;
  size_t weak_count = 0;
  unsigned entry_point = 0;
};

// Compute graph stats from in-memory adjacency list.
// nd = number of active (data) points; num_frozen_pts = 0 or 1.
// weak_threshold: nodes with degree < weak_threshold are counted as weak (default 2).
GraphStats compute_graph_stats(const std::vector<std::vector<unsigned>> &graph, size_t nd,
                               size_t num_frozen_pts, unsigned ep, unsigned weak_threshold = 2);

// Compute graph stats by reading the persisted graph format (same layout as save_graph/load_graph).
// path: graph file path; offset: byte offset where graph header starts (0 for raw graph file).
// Returns stats with total_nodes=0 on read error.
GraphStats compute_graph_stats_from_file(const std::string &path, size_t offset);

// Print the standard structural report to out.
void print_graph_report(const GraphStats &s, std::ostream &out);

// Read graph from file and print adjacency sample: first num_nodes nodes, each line "node_id: [n1, n2, ...]"
// with at most max_neighbors_per_node neighbors shown (0 = no cap).
// path and offset same as compute_graph_stats_from_file.
void print_adjacency_sample_from_file(const std::string &path, size_t offset, size_t num_nodes,
                                      size_t max_neighbors_per_node, std::ostream &out);

// Disk index data type (element size for vector coordinates).
enum class DiskIndexDataType { kFloat = 0, kUint8, kInt8 };

// Compute graph stats by reading the on-disk SSD index format (*_disk.index).
// Layout: 2*u32 + 5*u64 header, then data at 4096; each node = max_node_len bytes (coords + nnbrs + nbrs).
// Returns stats with total_nodes=0 on read error.
GraphStats compute_graph_stats_from_disk_index(const std::string &path, DiskIndexDataType data_type);

// Print adjacency sample from a disk index file (first num_nodes nodes).
void print_adjacency_sample_from_disk_index(const std::string &path, DiskIndexDataType data_type,
                                            size_t num_nodes, size_t max_neighbors_per_node, std::ostream &out);

// Small graph: first num_nodes with out-neighbors and "referenced by" (in-neighbors among those nodes).
// Same layout as compute_graph_stats_from_file / print_adjacency_sample_from_file.
void print_small_graph_from_file(const std::string &path, size_t offset, size_t num_nodes,
                                 size_t max_neighbors_per_node, std::ostream &out);

// Small graph from disk index: first num_nodes with out-neighbors and "referenced by" (in-neighbors among those nodes).
void print_small_graph_from_disk_index(const std::string &path, DiskIndexDataType data_type,
                                       size_t num_nodes, size_t max_neighbors_per_node, std::ostream &out);

}  // namespace pipeann
