// Serialization microbenchmark for ADR-002 (#41).
//
// Five wire formats encode and decode the same small record. The
// implementations below are deliberately self-contained: the point is to
// compare encoding strategies on this machine, not to benchmark the official
// protobuf / MessagePack / FlatBuffers runtimes. The ADR says so explicitly and
// explains which numbers transfer and which do not.
//
// Build and run:
//   cmake -B build -DCMAKE_BUILD_TYPE=Release
//   cmake --build build --target serialization_bench
//   ./build/serialization_bench --iterations 200000 --repeat 5
//
// Every number printed here is what that command produces.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct User {
  uint64_t id = 42;
  int32_t score = 87;
  bool active = true;
  std::vector<int32_t> tags{1, 2, 3, 4};
  std::string name = "alice";
  std::string email = "alice@example.com";
};

struct FlatView {
  uint64_t id = 0;
  int32_t score = 0;
  bool active = false;
  const char *tagsBytes = nullptr; // points into the encoded buffer
  uint8_t tagCount = 0;
  std::string_view name;
  std::string_view email;
};

// --- raw header + length-prefixed strings ----------------------------------

void putU32(std::string *out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
  }
}

void putU64(std::string *out, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
  }
}

uint32_t getU32(const char *p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  }
  return v;
}

uint64_t getU64(const char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  }
  return v;
}

bool rawEncode(const User &u, std::string *out) {
  out->clear();
  putU64(out, u.id);
  putU32(out, static_cast<uint32_t>(u.score));
  out->push_back(u.active ? 1 : 0);
  out->push_back(static_cast<char>(u.tags.size()));
  for (int32_t tag : u.tags) {
    putU32(out, static_cast<uint32_t>(tag));
  }
  putU32(out, static_cast<uint32_t>(u.name.size()));
  out->append(u.name);
  putU32(out, static_cast<uint32_t>(u.email.size()));
  out->append(u.email);
  return true;
}

bool rawDecode(std::string_view in, User *u) {
  const char *p = in.data();
  const char *end = p + in.size();
  if (end - p < 14 || static_cast<uint8_t>(p[12]) > 64) {
    return false;
  }
  u->id = getU64(p);
  u->score = static_cast<int32_t>(getU32(p + 8));
  u->active = p[12] != 0;
  const uint8_t tagCount = static_cast<uint8_t>(p[13]);
  p += 14;
  if (end - p < static_cast<ptrdiff_t>(tagCount) * 4 + 4) {
    return false;
  }
  u->tags.clear();
  for (uint8_t i = 0; i < tagCount; ++i, p += 4) {
    u->tags.push_back(static_cast<int32_t>(getU32(p)));
  }
  const uint32_t nameLen = getU32(p);
  p += 4;
  if (end - p < static_cast<ptrdiff_t>(nameLen) + 4) {
    return false;
  }
  u->name.assign(p, nameLen);
  p += nameLen;
  const uint32_t emailLen = getU32(p);
  p += 4;
  if (end - p < static_cast<ptrdiff_t>(emailLen)) {
    return false;
  }
  u->email.assign(p, emailLen);
  return true;
}

// --- JSON (hand-written writer, fixed-schema scanner) ----------------------

void jsonEncode(const User &u, std::string *out) {
  out->clear();
  out->append("{\"id\":");
  out->append(std::to_string(u.id));
  out->append(",\"score\":");
  out->append(std::to_string(u.score));
  out->append(",\"active\":");
  out->append(u.active ? "true" : "false");
  out->append(",\"tags\":[");
  for (size_t i = 0; i < u.tags.size(); ++i) {
    if (i != 0) {
      out->push_back(',');
    }
    out->append(std::to_string(u.tags[i]));
  }
  out->append("],\"name\":\"");
  out->append(u.name); // benchmark data contains no escapes
  out->append("\",\"email\":\"");
  out->append(u.email);
  out->append("\"}");
}

bool jsonDecode(std::string_view in, User *u) {
  auto after = [&](std::string_view key) -> size_t {
    const size_t at = in.find(key);
    return at == std::string_view::npos ? std::string_view::npos
                                        : at + key.size();
  };
  auto readInt = [&](std::string_view key, int64_t *out) -> bool {
    const size_t at = after(key);
    if (at == std::string_view::npos || at >= in.size()) {
      return false;
    }
    size_t p = at;
    bool negative = false;
    if (in[p] == '-') {
      negative = true;
      ++p;
    }
    int64_t v = 0;
    while (p < in.size() && in[p] >= '0' && in[p] <= '9') {
      v = v * 10 + (in[p] - '0');
      ++p;
    }
    *out = negative ? -v : v;
    return true;
  };
  auto readString = [&](std::string_view key, std::string *out) -> bool {
    const size_t at = after(key);
    if (at == std::string_view::npos || at >= in.size() || in[at] != '"') {
      return false;
    }
    const size_t close = in.find('"', at + 1);
    if (close == std::string_view::npos) {
      return false;
    }
    out->assign(in.data() + at + 1, close - at - 1);
    return true;
  };

  int64_t id = 0;
  int64_t score = 0;
  if (!readInt("\"id\":", &id) || !readInt("\"score\":", &score)) {
    return false;
  }
  u->id = static_cast<uint64_t>(id);
  u->score = static_cast<int32_t>(score);
  u->active = in.find("\"active\":true") != std::string_view::npos;

  u->tags.clear();
  const size_t tagsAt = after("\"tags\":[");
  if (tagsAt == std::string_view::npos) {
    return false;
  }
  size_t p = tagsAt;
  while (p < in.size() && in[p] != ']') {
    if (in[p] >= '0' && in[p] <= '9') {
      int64_t v = 0;
      while (p < in.size() && in[p] >= '0' && in[p] <= '9') {
        v = v * 10 + (in[p] - '0');
        ++p;
      }
      u->tags.push_back(static_cast<int32_t>(v));
    } else {
      ++p;
    }
  }
  return readString("\"name\":", &u->name) &&
         readString("\"email\":", &u->email);
}

// --- MessagePack subset ----------------------------------------------------

void mpPutStr(std::string *out, std::string_view s) {
  out->push_back(static_cast<char>(0xA0u | s.size()));
  out->append(s);
}

void mpPutUint(std::string *out, uint64_t v) {
  if (v < 128) {
    out->push_back(static_cast<char>(v));
  } else {
    out->push_back(static_cast<char>(0xCF)); // uint64
    for (int i = 7; i >= 0; --i) {
      out->push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    }
  }
}

void mpEncode(const User &u, std::string *out) {
  out->clear();
  out->push_back(static_cast<char>(0x86)); // fixmap, 6 entries
  mpPutStr(out, "id");
  mpPutUint(out, u.id);
  mpPutStr(out, "score");
  out->push_back(static_cast<char>(static_cast<uint8_t>(u.score) < 128
                                       ? static_cast<uint8_t>(u.score)
                                       : 0xD0));
  if (static_cast<uint8_t>(u.score) >= 128) {
    out->push_back(static_cast<char>(static_cast<uint8_t>(u.score)));
  }
  mpPutStr(out, "active");
  out->push_back(static_cast<char>(u.active ? 0xC3 : 0xC2));
  mpPutStr(out, "tags");
  out->push_back(static_cast<char>(0x94)); // fixarray, 4
  for (int32_t tag : u.tags) {
    out->push_back(static_cast<char>(tag)); // small positive ints only
  }
  mpPutStr(out, "name");
  mpPutStr(out, u.name);
  mpPutStr(out, "email");
  mpPutStr(out, u.email);
}

bool mpSkip(std::string_view in, size_t *p);

bool mpReadStr(std::string_view in, size_t *p, std::string *out) {
  if (*p >= in.size()) {
    return false;
  }
  const uint8_t marker = static_cast<uint8_t>(in[*p]);
  if ((marker & 0xE0u) != 0xA0u) {
    return false;
  }
  const size_t len = marker & 0x1Fu;
  if (*p + 1 + len > in.size()) {
    return false;
  }
  out->assign(in.data() + *p + 1, len);
  *p += 1 + len;
  return true;
}

bool mpSkip(std::string_view in, size_t *p) {
  if (*p >= in.size()) {
    return false;
  }
  const uint8_t marker = static_cast<uint8_t>(in[*p]);
  if (marker <= 0x7F) {
    *p += 1;
    return true;
  }
  if ((marker & 0xE0u) == 0xA0u) {
    *p += 1 + (marker & 0x1Fu);
    return *p <= in.size();
  }
  if (marker == 0xC2 || marker == 0xC3) {
    *p += 1;
    return true;
  }
  if (marker == 0xCF) {
    *p += 9;
    return *p <= in.size();
  }
  if (marker == 0xD0) {
    *p += 2;
    return *p <= in.size();
  }
  if ((marker & 0xF0u) == 0x90u) {
    const size_t n = marker & 0x0Fu;
    *p += 1;
    for (size_t i = 0; i < n; ++i) {
      if (!mpSkip(in, p)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

bool mpReadUint(std::string_view in, size_t *p, uint64_t *out) {
  if (*p >= in.size()) {
    return false;
  }
  const uint8_t marker = static_cast<uint8_t>(in[*p]);
  if (marker <= 0x7F) {
    *out = marker;
    *p += 1;
    return true;
  }
  if (marker == 0xCF) {
    if (*p + 9 > in.size()) {
      return false;
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v = (v << 8) | static_cast<uint8_t>(in[*p + 1 + i]);
    }
    *out = v;
    *p += 9;
    return true;
  }
  return false;
}

bool mpDecode(std::string_view in, User *u) {
  if (in.empty() || static_cast<uint8_t>(in[0]) != 0x86) {
    return false;
  }
  size_t p = 1;
  for (int field = 0; field < 6; ++field) {
    std::string key;
    if (!mpReadStr(in, &p, &key)) {
      return false;
    }
    if (key == "id") {
      if (!mpReadUint(in, &p, &u->id)) {
        return false;
      }
    } else if (key == "score") {
      if (p >= in.size()) {
        return false;
      }
      const uint8_t marker = static_cast<uint8_t>(in[p]);
      if (marker <= 0x7F) {
        u->score = marker;
        ++p;
      } else if (marker == 0xD0 && p + 2 <= in.size()) {
        u->score = static_cast<int8_t>(in[p + 1]);
        p += 2;
      } else {
        return false;
      }
    } else if (key == "active") {
      if (p >= in.size() ||
          (in[p] != static_cast<char>(0xC2) &&
           in[p] != static_cast<char>(0xC3))) {
        return false;
      }
      u->active = in[p] == static_cast<char>(0xC3);
      ++p;
    } else if (key == "tags") {
      if (p >= in.size() || (static_cast<uint8_t>(in[p]) & 0xF0u) != 0x90u) {
        return false;
      }
      const size_t n = static_cast<uint8_t>(in[p]) & 0x0Fu;
      ++p;
      u->tags.clear();
      for (size_t i = 0; i < n; ++i) {
        if (p >= in.size() ||
            static_cast<uint8_t>(in[p]) > 0x7F) {
          return false;
        }
        u->tags.push_back(static_cast<uint8_t>(in[p]));
        ++p;
      }
    } else if (key == "name") {
      if (!mpReadStr(in, &p, &u->name)) {
        return false;
      }
    } else if (key == "email") {
      if (!mpReadStr(in, &p, &u->email)) {
        return false;
      }
    } else if (!mpSkip(in, &p)) {
      return false;
    }
  }
  return true;
}

// --- protobuf wire format (hand-encoded, no libprotobuf) -------------------

void pbVarint(std::string *out, uint64_t v) {
  while (v >= 0x80u) {
    out->push_back(static_cast<char>((v & 0x7Fu) | 0x80u));
    v >>= 7;
  }
  out->push_back(static_cast<char>(v));
}

void pbTag(std::string *out, uint32_t field, uint32_t wireType) {
  pbVarint(out, (static_cast<uint64_t>(field) << 3) | wireType);
}

void pbString(std::string *out, uint32_t field, std::string_view s) {
  pbTag(out, field, 2);
  pbVarint(out, s.size());
  out->append(s);
}

void pbEncode(const User &u, std::string *out) {
  out->clear();
  pbTag(out, 1, 0); // id, varint
  pbVarint(out, u.id);
  pbString(out, 2, u.name);
  pbString(out, 3, u.email);
  pbTag(out, 4, 0); // active, varint
  pbVarint(out, u.active ? 1 : 0);
  pbTag(out, 5, 0); // score, varint
  pbVarint(out, static_cast<uint32_t>(u.score));
  // tags: packed repeated int32
  std::string packed;
  for (int32_t tag : u.tags) {
    pbVarint(&packed, static_cast<uint32_t>(tag));
  }
  pbTag(out, 6, 2);
  pbVarint(out, packed.size());
  out->append(packed);
}

bool pbReadVarint(std::string_view in, size_t *p, uint64_t *out) {
  uint64_t v = 0;
  int shift = 0;
  while (*p < in.size() && shift <= 63) {
    const uint8_t byte = static_cast<uint8_t>(in[*p]);
    ++*p;
    v |= static_cast<uint64_t>(byte & 0x7Fu) << shift;
    if ((byte & 0x80u) == 0) {
      *out = v;
      return true;
    }
    shift += 7;
  }
  return false;
}

bool pbDecode(std::string_view in, User *u) {
  size_t p = 0;
  while (p < in.size()) {
    uint64_t key = 0;
    if (!pbReadVarint(in, &p, &key)) {
      return false;
    }
    const uint32_t field = static_cast<uint32_t>(key >> 3);
    const uint32_t wire = static_cast<uint32_t>(key & 0x7u);
    if (wire == 0) {
      uint64_t v = 0;
      if (!pbReadVarint(in, &p, &v)) {
        return false;
      }
      if (field == 1) {
        u->id = v;
      } else if (field == 4) {
        u->active = v != 0;
      } else if (field == 5) {
        u->score = static_cast<int32_t>(v);
      }
    } else if (wire == 2) {
      uint64_t len = 0;
      if (!pbReadVarint(in, &p, &len) || p + len > in.size()) {
        return false;
      }
      const std::string_view bytes(in.data() + p, len);
      p += len;
      if (field == 2) {
        u->name.assign(bytes);
      } else if (field == 3) {
        u->email.assign(bytes);
      } else if (field == 6) {
        u->tags.clear();
        size_t q = 0;
        while (q < bytes.size()) {
          uint64_t v = 0;
          if (!pbReadVarint(bytes, &q, &v)) {
            return false;
          }
          u->tags.push_back(static_cast<int32_t>(v));
        }
      }
    } else if (wire == 5) {
      p += 4;
    } else if (wire == 1) {
      p += 8;
    } else {
      return false;
    }
  }
  return true;
}

// --- FlatBuffers-style layout (hand-rolled, zero-copy decode) --------------

constexpr size_t kFlatTagsOffset = 16;
constexpr size_t kFlatOffsets = 32;
constexpr size_t kFlatHeader = 40;

void flatEncode(const User &u, std::string *out) {
  out->clear();
  out->resize(kFlatHeader);
  std::memcpy(out->data(), &u.id, 8);
  std::memcpy(out->data() + 8, &u.score, 4);
  (*out)[12] = u.active ? 1 : 0;
  (*out)[13] = static_cast<char>(u.tags.size());
  for (size_t i = 0; i < u.tags.size() && i < 4; ++i) {
    std::memcpy(out->data() + kFlatTagsOffset + i * 4, &u.tags[i], 4);
  }
  const uint32_t nameOff = static_cast<uint32_t>(out->size());
  out->append(u.name);
  const uint32_t emailOff = static_cast<uint32_t>(out->size());
  out->append(u.email);
  std::memcpy(out->data() + kFlatOffsets, &nameOff, 4);
  std::memcpy(out->data() + kFlatOffsets + 4, &emailOff, 4);
}

bool flatDecode(std::string_view in, FlatView *v) {
  if (in.size() < kFlatHeader) {
    return false;
  }
  std::memcpy(&v->id, in.data(), 8);
  std::memcpy(&v->score, in.data() + 8, 4);
  v->active = in[12] != 0;
  const uint8_t tagCount = static_cast<uint8_t>(in[13]);
  if (tagCount > 4) {
    return false;
  }
  // Zero-copy: the view points at the packed ints, it does not copy them.
  v->tagsBytes = in.data() + kFlatTagsOffset;
  v->tagCount = tagCount;
  uint32_t nameOff = 0;
  uint32_t emailOff = 0;
  std::memcpy(&nameOff, in.data() + kFlatOffsets, 4);
  std::memcpy(&emailOff, in.data() + kFlatOffsets + 4, 4);
  if (nameOff > in.size() || emailOff > in.size() ||
      emailOff < nameOff) {
    return false;
  }
  v->name = in.substr(nameOff, emailOff - nameOff);
  v->email = in.substr(emailOff);
  return true;
}

// --- harness ----------------------------------------------------------------

volatile uint64_t gSink = 0;

uint64_t rawChecksum(const User &u) {
  uint64_t sum = u.id + static_cast<uint64_t>(u.score) + (u.active ? 1 : 0) +
                 u.name.size() + u.email.size();
  for (int32_t tag : u.tags) {
    sum += static_cast<uint64_t>(tag);
  }
  return sum;
}

uint64_t flatChecksum(const FlatView &v) {
  uint64_t sum = v.id + static_cast<uint64_t>(v.score) +
                 (v.active ? 1 : 0) + v.name.size() + v.email.size();
  for (uint8_t i = 0; i < v.tagCount; ++i) {
    int32_t tag = 0;
    std::memcpy(&tag, v.tagsBytes + i * 4, 4);
    sum += static_cast<uint64_t>(tag);
  }
  return sum;
}

bool sameUser(const User &a, const User &b) {
  return a.id == b.id && a.score == b.score && a.active == b.active &&
         a.tags == b.tags && a.name == b.name && a.email == b.email;
}

bool sameUser(const FlatView &a, const User &b) {
  if (a.id != b.id || a.score != b.score || a.active != b.active ||
      a.tagCount != b.tags.size() || a.name != b.name ||
      a.email != b.email) {
    return false;
  }
  for (size_t i = 0; i < b.tags.size(); ++i) {
    int32_t tag = 0;
    std::memcpy(&tag, a.tagsBytes + i * 4, 4);
    if (tag != b.tags[i]) {
      return false;
    }
  }
  return true;
}

template <typename EncodeFn>
size_t encodeOnce(const User &u, EncodeFn encode, std::string *buffer) {
  encode(u, buffer);
  return buffer->size();
}

// Best of `repeat` runs, in operations per second. The encode result feeds a
// volatile sink so the compiler cannot discard the work.
template <typename EncodeFn>
double timeEncode(int iterations, int repeat, const User &u, EncodeFn encode,
                  size_t *bytesOut) {
  std::string buffer;
  *bytesOut = encodeOnce(u, encode, &buffer);
  double best = 0.0;
  for (int r = 0; r < repeat; ++r) {
    uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
      encode(u, &buffer);
      sum += buffer.size();
    }
    const auto end = std::chrono::steady_clock::now();
    gSink = sum;
    const double seconds =
        std::chrono::duration<double>(end - start).count();
    best = std::max(best, iterations / seconds);
  }
  return best;
}

template <typename Decoded, typename DecodeFn, typename ChecksumFn>
double timeDecode(int iterations, int repeat, std::string_view encoded,
                  DecodeFn decode, ChecksumFn checksum) {
  double best = 0.0;
  for (int r = 0; r < repeat; ++r) {
    uint64_t sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
      Decoded out{};
      if (!decode(encoded, &out)) {
        return 0.0;
      }
      sum += checksum(out);
    }
    const auto end = std::chrono::steady_clock::now();
    gSink = sum;
    const double seconds =
        std::chrono::duration<double>(end - start).count();
    best = std::max(best, iterations / seconds);
  }
  return best;
}

struct BenchRow {
  const char *name;
  double encodeMops;
  double decodeMops;
  size_t bytes;
};

void printRows(const std::vector<BenchRow> &rows) {
  std::printf("%-18s %14s %14s %10s\n", "format", "encode Mop/s",
              "decode Mop/s", "bytes");
  for (const BenchRow &row : rows) {
    std::printf("%-18s %14.2f %14.2f %10zu\n", row.name, row.encodeMops,
                row.decodeMops, row.bytes);
  }
}

} // namespace

int main(int argc, char **argv) {
  int iterations = 200000;
  int repeat = 5;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--iterations" && i + 1 < argc) {
      iterations = std::atoi(argv[++i]);
    } else if (arg == "--repeat" && i + 1 < argc) {
      repeat = std::atoi(argv[++i]);
    } else {
      std::fprintf(stderr, "usage: %s [--iterations N] [--repeat N]\n",
                   argv[0]);
      return 2;
    }
  }
  if (iterations <= 0 || repeat <= 0) {
    std::fprintf(stderr, "iterations and repeat must be positive\n");
    return 2;
  }

  const User user;
  std::vector<BenchRow> rows;
  rows.reserve(5);

  {
    std::string encoded;
    rawEncode(user, &encoded);
    User decoded;
    if (!rawDecode(encoded, &decoded) || !sameUser(decoded, user)) {
      std::fprintf(stderr, "raw codec failed its round trip\n");
      return 1;
    }
    const double enc =
        timeEncode(iterations, repeat, user, rawEncode, &rows.emplace_back().bytes);
    rows.back().name = "raw binary";
    rows.back().encodeMops = enc / 1e6;
    rows.back().decodeMops =
        timeDecode<User>(iterations, repeat, encoded, rawDecode,
                         rawChecksum) /
        1e6;
  }
  {
    std::string encoded;
    jsonEncode(user, &encoded);
    User decoded;
    if (!jsonDecode(encoded, &decoded) || !sameUser(decoded, user)) {
      std::fprintf(stderr, "json codec failed its round trip\n");
      return 1;
    }
    const double enc = timeEncode(iterations, repeat, user, jsonEncode,
                                  &rows.emplace_back().bytes);
    rows.back().name = "json";
    rows.back().encodeMops = enc / 1e6;
    rows.back().decodeMops =
        timeDecode<User>(iterations, repeat, encoded, jsonDecode,
                         rawChecksum) /
        1e6;
  }
  {
    std::string encoded;
    mpEncode(user, &encoded);
    User decoded;
    if (!mpDecode(encoded, &decoded) || !sameUser(decoded, user)) {
      std::fprintf(stderr, "msgpack codec failed its round trip\n");
      return 1;
    }
    const double enc = timeEncode(iterations, repeat, user, mpEncode,
                                  &rows.emplace_back().bytes);
    rows.back().name = "msgpack";
    rows.back().encodeMops = enc / 1e6;
    rows.back().decodeMops =
        timeDecode<User>(iterations, repeat, encoded, mpDecode, rawChecksum) /
        1e6;
  }
  {
    std::string encoded;
    pbEncode(user, &encoded);
    User decoded;
    if (!pbDecode(encoded, &decoded) || !sameUser(decoded, user)) {
      std::fprintf(stderr, "protobuf codec failed its round trip\n");
      return 1;
    }
    const double enc = timeEncode(iterations, repeat, user, pbEncode,
                                  &rows.emplace_back().bytes);
    rows.back().name = "protobuf wire";
    rows.back().encodeMops = enc / 1e6;
    rows.back().decodeMops =
        timeDecode<User>(iterations, repeat, encoded, pbDecode, rawChecksum) /
        1e6;
  }
  {
    std::string encoded;
    flatEncode(user, &encoded);
    FlatView decoded;
    if (!flatDecode(encoded, &decoded) || !sameUser(decoded, user)) {
      std::fprintf(stderr, "flatbuffers-style codec failed its round trip\n");
      return 1;
    }
    const double enc = timeEncode(iterations, repeat, user, flatEncode,
                                  &rows.emplace_back().bytes);
    rows.back().name = "flatbuffers-style";
    rows.back().encodeMops = enc / 1e6;
    rows.back().decodeMops =
        timeDecode<FlatView>(iterations, repeat, encoded, flatDecode,
                             flatChecksum) /
        1e6;
  }

  printRows(rows);
  std::printf("\n%d iterations x %d repeats; checksum=%llu\n", iterations,
              repeat, static_cast<unsigned long long>(gSink));
  return 0;
}
