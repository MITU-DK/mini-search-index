#include "bktree.hpp"

#include <algorithm>

#include "spellcheck.hpp"

namespace mse {
namespace {

// A node while the tree is being built: its children are added as terms arrive.
struct BuildNode {
  std::string_view term;
  uint32_t term_index;
  std::vector<std::pair<uint32_t, uint32_t>> children;  // (distance, node number)
};

}  // namespace

FlatBkTree build_bktree(std::span<const std::pair<std::string_view, uint32_t>> terms) {
  std::vector<BuildNode> nodes;
  nodes.reserve(terms.size());
  for (const auto& [term, term_index] : terms) {
    if (nodes.empty()) {
      nodes.push_back({term, term_index, {}});
      continue;
    }
    // Walk down from the root: at each node follow the edge labelled with the distance to it,
    // and hang the term on the first node that has no such edge.
    uint32_t at = 0;
    while (true) {
      const uint32_t distance = levenshtein(term, nodes[at].term);
      if (distance == 0) break;  // the same term twice: nothing to add
      const auto edge = std::find_if(nodes[at].children.begin(), nodes[at].children.end(),
                                     [&](const auto& child) { return child.first == distance; });
      if (edge != nodes[at].children.end()) {
        at = edge->second;
        continue;
      }
      const auto child = static_cast<uint32_t>(nodes.size());
      nodes[at].children.emplace_back(distance, child);  // (invalidates references, so index)
      nodes.push_back({term, term_index, {}});
      break;
    }
  }

  // Flatten breadth first. A node's children get the next numbers when the node is reached, so
  // they are consecutive, and flat_number == position in `order`.
  FlatBkTree flat;
  flat.nodes.reserve(nodes.size());
  flat.edges.reserve(nodes.empty() ? 0 : nodes.size() - 1);
  std::vector<uint32_t> order;  // build node numbers, in flat order
  order.reserve(nodes.size());
  if (!nodes.empty()) order.push_back(0);
  for (std::size_t position = 0; position < order.size(); ++position) {
    BuildNode& node = nodes[order[position]];
    std::sort(node.children.begin(), node.children.end());
    flat.nodes.push_back({node.term_index, static_cast<uint32_t>(flat.edges.size()),
                          static_cast<uint32_t>(node.children.size())});
    for (const auto& [distance, child] : node.children) {
      flat.edges.push_back({distance, static_cast<uint32_t>(order.size())});
      order.push_back(child);
    }
  }
  return flat;
}

}  // namespace mse
