#include "index_writer.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>

#include "file_descriptor.hpp"
#include "varint.hpp"

namespace mse {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kMaxU32 = std::numeric_limits<uint32_t>::max();
constexpr std::size_t kCopyBufferSize = 1 << 20;

std::string errno_text() { return std::strerror(errno); }

void put_u32(std::vector<std::byte>& out, uint32_t value) {
  out.resize(out.size() + sizeof(value));
  store_le<uint32_t>(out, out.size() - sizeof(value), value);
}

void put_u64(std::vector<std::byte>& out, uint64_t value) {
  out.resize(out.size() + sizeof(value));
  store_le<uint64_t>(out, out.size() - sizeof(value), value);
}

std::expected<void, std::string> check_postings(std::span<const Posting> postings,
                                                uint32_t num_docs) {
  if (postings.empty()) return std::unexpected("a term needs at least one posting");
  if (postings.size() > kMaxU32) return std::unexpected("too many postings for one term");
  for (std::size_t i = 0; i < postings.size(); ++i) {
    if (postings[i].tf == 0) return std::unexpected("a posting has a frequency of 0");
    if (postings[i].doc >= num_docs)
      return std::unexpected("a posting names a doc that is not in the index");
    if (i > 0 && postings[i].doc <= postings[i - 1].doc) {
      return std::unexpected("postings are not in strictly increasing doc order");
    }
  }
  return {};
}

void encode_block(std::span<const Posting> postings, bool compress, std::vector<std::byte>& out) {
  out.clear();
  if (compress) {
    encode_postings(postings, out);
    return;
  }
  for (const Posting& posting : postings) {
    put_u32(out, posting.doc);
    put_u32(out, posting.tf);
  }
}

// ---- assembling the file ----

std::expected<void, std::string> pwrite_all(int fd, uint64_t offset,
                                            std::span<const std::byte> data) {
  while (!data.empty()) {
    const ssize_t written = ::pwrite(fd, data.data(), data.size(), static_cast<off_t>(offset));
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return std::unexpected("write failed: " + errno_text());
    data = data.subspan(static_cast<std::size_t>(written));
    offset += static_cast<uint64_t>(written);
  }
  return {};
}

// Copies with read and write, for filesystems that cannot copy_file_range.
std::expected<void, std::string> copy_with_buffer(int in_fd, uint64_t in_offset, int out_fd,
                                                  uint64_t out_offset, uint64_t size) {
  std::vector<std::byte> buffer(kCopyBufferSize);
  while (size > 0) {
    const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(size, buffer.size()));
    const ssize_t got = ::pread(in_fd, buffer.data(), want, static_cast<off_t>(in_offset));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return std::unexpected("temporary file is shorter than expected");
    const auto chunk = std::span<const std::byte>(buffer).first(static_cast<std::size_t>(got));
    if (auto ok = pwrite_all(out_fd, out_offset, chunk); !ok) return ok;
    in_offset += static_cast<uint64_t>(got);
    out_offset += static_cast<uint64_t>(got);
    size -= static_cast<uint64_t>(got);
  }
  return {};
}

// Appends the first `size` bytes of `source` to the output at `out_offset`.
std::expected<void, std::string> copy_file_into(int out_fd, uint64_t out_offset,
                                                const fs::path& source, uint64_t size) {
  const FileDescriptor in(::open(source.c_str(), O_RDONLY));
  if (in.get() < 0) return std::unexpected("cannot open " + source.string() + ": " + errno_text());

  uint64_t done = 0;
  while (done < size) {
    off_t in_off = static_cast<off_t>(done);
    off_t out_off = static_cast<off_t>(out_offset + done);
    const ssize_t n = ::copy_file_range(in.get(), &in_off, out_fd, &out_off, size - done, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
      return copy_with_buffer(in.get(), done, out_fd, out_offset + done, size - done);
    }
    if (n < 0) return std::unexpected("copy failed: " + errno_text());
    if (n == 0) return std::unexpected("temporary file is shorter than expected");
    done += static_cast<uint64_t>(n);
  }
  return {};
}

// Gives each section an 8-aligned offset after the one before it, and sets file_size.
void assign_layout(Header& header) {
  uint64_t cursor = kHeaderSize;
  for (SectionRef& ref : header.sections) {
    ref.offset = align_up(cursor);
    cursor = ref.offset + ref.size;
  }
  header.file_size = cursor;
}

}  // namespace

std::expected<IndexWriter, std::string> IndexWriter::create(const fs::path& out,
                                                            const WriteOptions& options) {
  IndexWriter writer;
  writer.out_ = out;
  writer.options_ = options;
  writer.started_ = std::chrono::steady_clock::now();
  writer.tmp_path_ = out.string() + ".tmp";
  writer.postings_path_ = out.string() + ".postings.tmp";
  writer.terms_path_ = out.string() + ".terms.tmp";
  writer.strings_path_ = out.string() + ".tstr.tmp";

  for (const fs::path* path :
       {&writer.tmp_path_, &writer.postings_path_, &writer.terms_path_, &writer.strings_path_}) {
    writer.temp_files_.add(*path);
  }
  const auto open = [](std::ofstream& stream, const fs::path& path) {
    stream.open(path, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(stream);
  };
  for (auto [stream, path] : {std::pair{&writer.postings_, &writer.postings_path_},
                              std::pair{&writer.terms_, &writer.terms_path_},
                              std::pair{&writer.strings_, &writer.strings_path_}}) {
    if (!open(*stream, *path)) return std::unexpected("cannot create " + path->string());
  }
  return writer;
}

std::expected<uint32_t, std::string> IndexWriter::append_string(std::string_view text) {
  if (strings_bytes_ + text.size() > kMaxU32) {
    return std::unexpected("the strings section would be larger than 4 GiB");
  }
  const auto offset = static_cast<uint32_t>(strings_bytes_);
  strings_.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!strings_) return std::unexpected("write to " + strings_path_.string() + " failed");
  strings_bytes_ += text.size();
  return offset;
}

std::expected<void, std::string> IndexWriter::set_docs(std::string_view corpus_root,
                                                       std::span<const std::string> part_names,
                                                       std::span<const DocInfo> docs) {
  if (docs_set_) return std::unexpected("set_docs was already called");
  if (docs.size() > kMaxU32 || part_names.size() > kMaxU32) {
    return std::unexpected("too many docs or parts for this format");
  }
  for (const DocInfo& doc : docs) {
    if (doc.ref.part >= part_names.size())
      return std::unexpected("a doc names a part that does not exist");
  }

  const auto root = append_string(corpus_root);
  if (!root) return std::unexpected(root.error());
  header_.corpus_root_offset = *root;
  header_.corpus_root_length = static_cast<uint32_t>(corpus_root.size());

  for (const std::string& name : part_names) {
    const auto offset = append_string(name);
    if (!offset) return std::unexpected(offset.error());
    put_u32(part_names_, *offset);
    put_u32(part_names_, static_cast<uint32_t>(name.size()));
  }

  uint64_t total_tokens = 0;
  for (const DocInfo& doc : docs) {
    const auto offset = append_string(doc.title);
    if (!offset) return std::unexpected(offset.error());
    put_u32(doc_titles_, *offset);
    put_u32(doc_titles_, static_cast<uint32_t>(doc.title.size()));
    put_u32(doc_lengths_, doc.length);
    put_u32(doc_refs_, doc.ref.part);
    put_u32(doc_refs_, doc.ref.length);
    put_u64(doc_refs_, doc.ref.offset);
    total_tokens += doc.length;
  }

  header_.num_docs = static_cast<uint32_t>(docs.size());
  header_.num_parts = static_cast<uint32_t>(part_names.size());
  header_.avg_doc_len =
      docs.empty() ? 0.0 : static_cast<double>(total_tokens) / static_cast<double>(docs.size());
  docs_set_ = true;
  return {};
}

std::expected<void, std::string> IndexWriter::add_term(std::string_view term,
                                                       std::span<const Posting> postings) {
  if (!docs_set_) return std::unexpected("set_docs must be called before add_term");
  if (term.empty()) return std::unexpected("a term cannot be empty");
  if (num_terms_ > 0 && term.compare(last_term_) <= 0) {
    return std::unexpected("terms must be added in strictly increasing order: \"" +
                           std::string(term) + "\" after \"" + last_term_ + "\"");
  }
  if (num_terms_ == kMaxU32) return std::unexpected("too many terms for this format");
  if (auto ok = check_postings(postings, header_.num_docs); !ok) return ok;

  encode_block(postings, options_.compress, block_);
  if (block_.size() > kMaxU32) return std::unexpected("one term's postings are larger than 4 GiB");
  const auto str_off = append_string(term);
  if (!str_off) return std::unexpected(str_off.error());

  std::array<std::byte, kTermEntrySize> entry;
  encode_term_entry({.str_off = *str_off,
                     .str_len = static_cast<uint32_t>(term.size()),
                     .df = static_cast<uint32_t>(postings.size()),
                     .postings_size = static_cast<uint32_t>(block_.size()),
                     .postings_off = postings_bytes_},
                    entry);
  terms_.write(reinterpret_cast<const char*>(entry.data()), entry.size());
  postings_.write(reinterpret_cast<const char*>(block_.data()),
                  static_cast<std::streamsize>(block_.size()));
  if (!terms_ || !postings_) return std::unexpected("write to a temporary file failed");

  postings_bytes_ += block_.size();
  total_postings_ += postings.size();
  last_term_.assign(term);
  ++num_terms_;
  return {};
}

std::expected<void, std::string> IndexWriter::close_streams() {
  for (std::ofstream* stream : {&postings_, &terms_, &strings_}) {
    stream->close();
    if (!*stream) return std::unexpected("could not finish writing a temporary file (disk full?)");
  }
  return {};
}

std::expected<void, std::string> IndexWriter::assemble(const Header& header) {
  const FileDescriptor out(::open(tmp_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
  if (out.get() < 0)
    return std::unexpected("cannot create " + tmp_path_.string() + ": " + errno_text());
  if (::ftruncate(out.get(), static_cast<off_t>(header.file_size)) != 0) {
    return std::unexpected("cannot size " + tmp_path_.string() + ": " + errno_text());
  }

  std::array<std::byte, kHeaderSize> head;
  encode_header(header, head);
  if (auto ok = pwrite_all(out.get(), 0, head); !ok) return ok;

  const std::pair<Section, const std::vector<std::byte>*> small_sections[] = {
      {Section::DocLengths, &doc_lengths_},
      {Section::DocRefs, &doc_refs_},
      {Section::DocTitles, &doc_titles_},
      {Section::PartNames, &part_names_}};
  for (const auto& [section, bytes] : small_sections) {
    if (auto ok = pwrite_all(out.get(), header.section(section).offset, *bytes); !ok) return ok;
  }

  const std::pair<Section, const fs::path*> streamed_sections[] = {
      {Section::Terms, &terms_path_},
      {Section::Strings, &strings_path_},
      {Section::Postings, &postings_path_}};
  for (const auto& [section, path] : streamed_sections) {
    const SectionRef& ref = header.section(section);
    if (auto ok = copy_file_into(out.get(), ref.offset, *path, ref.size); !ok) return ok;
  }
  if (::fsync(out.get()) != 0) return std::unexpected("fsync failed: " + errno_text());
  return {};
}

std::expected<WriteStats, std::string> IndexWriter::finish() {
  if (!docs_set_) return std::unexpected("set_docs must be called before finish");
  if (finished_) return std::unexpected("finish was already called");
  if (auto ok = close_streams(); !ok) return std::unexpected(ok.error());

  Header header = header_;
  header.flags = options_.compress ? kFlagVByte : 0;
  header.num_terms = num_terms_;
  header.total_postings = total_postings_;
  header.section(Section::DocLengths).size = doc_lengths_.size();
  header.section(Section::DocRefs).size = doc_refs_.size();
  header.section(Section::DocTitles).size = doc_titles_.size();
  header.section(Section::PartNames).size = part_names_.size();
  header.section(Section::Terms).size = uint64_t{num_terms_} * kTermEntrySize;
  header.section(Section::Strings).size = strings_bytes_;
  header.section(Section::Postings).size = postings_bytes_;
  assign_layout(header);

  if (auto ok = assemble(header); !ok) return std::unexpected(ok.error());
  std::error_code error;
  fs::rename(tmp_path_, out_, error);
  if (error) return std::unexpected("cannot rename to " + out_.string() + ": " + error.message());
  finished_ = true;
  temp_files_.remove_all();

  return WriteStats{
      .file_bytes = header.file_size,
      .postings_bytes = postings_bytes_,
      .raw_postings_bytes = total_postings_ * kRawPostingSize,
      .write_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count(),
      .spell_build_s = 0};
}

std::expected<WriteStats, std::string> write_index(const InMemoryIndex& index, const fs::path& out,
                                                   const WriteOptions& options) {
  auto writer = IndexWriter::create(out, options);
  if (!writer) return std::unexpected(writer.error());
  if (auto ok = writer->set_docs(index.corpus_root, index.part_names, index.docs); !ok) {
    return std::unexpected(ok.error());
  }

  std::vector<const InMemoryIndex::PostingsMap::value_type*> sorted;
  sorted.reserve(index.postings.size());
  for (const auto& entry : index.postings) sorted.push_back(&entry);
  std::sort(sorted.begin(), sorted.end(),
            [](const auto* a, const auto* b) { return a->first < b->first; });

  for (const auto* entry : sorted) {
    if (auto ok = writer->add_term(entry->first, entry->second); !ok)
      return std::unexpected(ok.error());
  }
  return writer->finish();
}

}  // namespace mse
