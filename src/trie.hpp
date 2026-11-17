// A prefix tree over the index's terms, built to compare with the sorted-table autocomplete
// (DESIGN.md §4). Every node remembers the best few terms below it, so a query is a walk down the
// prefix and nothing more. The price is build time and memory; see benchmarks/NOTES.md (B7).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "constants.hpp"
#include "index_view.hpp"

namespace mse {

class Trie {
 public:
  // Reads every term of `index`. The trie borrows the view (it keeps term numbers, not strings),
  // so the view must outlive it.
  static Trie build(const IndexView& index);

  // Same answers as suggest() in autocomplete.hpp. At most kSuggestTopK terms are remembered per
  // node, so a larger `k` gives kSuggestTopK terms.
  std::vector<std::string> suggest(std::string_view prefix, std::size_t k = kSuggestTopK) const;

  // The bytes taken by the nodes: what the trie costs on top of the index.
  std::size_t memory_bytes() const { return nodes_.size() * sizeof(Node); }

 private:
  struct Best {
    uint32_t df;
    uint32_t term;  // position in the term table
  };
  struct Node {
    uint32_t first_child = 0;   // 0 means none: the root is never a child
    uint32_t next_sibling = 0;  // the children of a node form a list in increasing byte order
    uint8_t byte = 0;           // the byte on the edge into this node
    uint8_t best_count = 0;
    Best best[kSuggestTopK];  // the top terms below this node, best first
  };

  explicit Trie(const IndexView& index) : index_(&index) {}
  void add_term(uint32_t term, std::vector<uint32_t>& last_child);
  static void remember(Node& node, Best candidate);
  uint32_t find_child(uint32_t node, uint8_t byte) const;

  const IndexView* index_;
  std::vector<Node> nodes_;
};

}  // namespace mse
