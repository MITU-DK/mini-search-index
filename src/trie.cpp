#include "trie.hpp"

#include "tokenizer.hpp"

namespace mse {

Trie Trie::build(const IndexView& index) {
  Trie trie(index);
  trie.nodes_.emplace_back();              // the root
  std::vector<uint32_t> last_child(1, 0);  // only needed while building
  for (uint32_t term = 0; term < index.num_terms(); ++term) {
    trie.add_term(term, last_child);
    last_child.resize(trie.nodes_.size(), 0);
  }
  trie.nodes_.shrink_to_fit();
  return trie;
}

// Terms arrive in sorted order, so a term's next byte at a node is either the byte of the node's
// newest child or a new, larger one: the children list is kept sorted by appending to it.
void Trie::add_term(uint32_t term, std::vector<uint32_t>& last_child) {
  const std::string_view text = index_->term_at(term);
  const Best candidate{index_->df_at(term), term};
  uint32_t node = 0;
  for (const char c : text) {
    const auto byte = static_cast<uint8_t>(c);
    last_child.resize(nodes_.size(), 0);
    uint32_t child = last_child[node];
    if (child == 0 || nodes_[child].byte != byte) {
      child = static_cast<uint32_t>(nodes_.size());
      nodes_.emplace_back();
      nodes_[child].byte = byte;
      if (last_child[node] == 0) {
        nodes_[node].first_child = child;
      } else {
        nodes_[last_child[node]].next_sibling = child;
      }
      last_child[node] = child;
    }
    node = child;
    remember(nodes_[node], candidate);
  }
}

// Keeps the node's list sorted: more docs first, and for equal counts the lower term number
// first. A new term has the highest number so far, so it goes after the terms with the same df.
void Trie::remember(Node& node, Best candidate) {
  std::size_t position = node.best_count;
  while (position > 0 && node.best[position - 1].df < candidate.df) --position;
  if (position >= kSuggestTopK) return;

  const std::size_t last = std::min<std::size_t>(node.best_count, kSuggestTopK - 1);
  for (std::size_t i = last; i > position; --i) node.best[i] = node.best[i - 1];
  node.best[position] = candidate;
  if (node.best_count < kSuggestTopK) ++node.best_count;
}

uint32_t Trie::find_child(uint32_t node, uint8_t byte) const {
  for (uint32_t child = nodes_[node].first_child; child != 0; child = nodes_[child].next_sibling) {
    if (nodes_[child].byte == byte) return child;
  }
  return 0;
}

std::vector<std::string> Trie::suggest(std::string_view prefix, std::size_t k) const {
  const std::string normalized = normalize_term(prefix);
  if (k == 0 || normalized.size() < kSuggestMinPrefix) return {};

  uint32_t node = 0;
  for (const char c : normalized) {
    node = find_child(node, static_cast<uint8_t>(c));
    if (node == 0) return {};
  }
  std::vector<std::string> terms;
  const std::size_t count = std::min<std::size_t>(k, nodes_[node].best_count);
  for (std::size_t i = 0; i < count; ++i) {
    terms.emplace_back(index_->term_at(nodes_[node].best[i].term));
  }
  return terms;
}

}  // namespace mse
