// A BK-tree over the index's spell-check terms (DESIGN.md §4): a tree in which every edge is
// labelled with the edit distance between the two terms it joins. A search for words within
// distance `n` of a word at distance `d` from a node only has to follow the edges labelled
// d - n .. d + n, because of the triangle inequality, and so skips most of the tree.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "index_format.hpp"

namespace mse {

// The tree as it is stored in the file: node 0 is the root, nodes are in breadth-first order, and
// each node's edges are consecutive and sorted by distance.
struct FlatBkTree {
  std::vector<SpellNode> nodes;
  std::vector<SpellEdge> edges;
};

// Builds the tree from (term, term table index) pairs, inserting them in the order given; the
// first one becomes the root. The terms must all be different. No terms give an empty tree.
FlatBkTree build_bktree(std::span<const std::pair<std::string_view, uint32_t>> terms);

}  // namespace mse
