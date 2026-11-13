#include "sdk/nights_ui_images.hpp"

#include "ui/game_assets.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dualize::nights
{
namespace
{
constexpr const char* FOLDER = "files/csd/textures";
constexpr uint32_t MAX_SCALE = 4;
// Most texels of a copy's base level (1024x512 images are drawn twice as
// large, smaller ones four times).
constexpr uint64_t MAX_TEXELS = 4u << 20;
// Copies held, made and registered (aurora keeps the latter's texels).
constexpr uint64_t BUDGET = 512ull << 20;
constexpr unsigned WORKERS = 4;
// A file read again within this is the same load (it comes in pieces).
constexpr auto SAME_LOAD = std::chrono::seconds(2);

using Key = aurora::texture::TextureSourceKey;
struct KeyHash
{
  size_t operator()(const Key& k) const noexcept
  {
    return static_cast<size_t>(k.textureHash ^ (static_cast<uint64_t>(k.width) << 32 | k.height) * 0x9E3779B97F4A7C15ull ^
                               k.format);
  }
};

struct Copy
{
  // GONE: let go over budget; made again from its file when next drawn.
  enum State : uint8_t { QUEUED, MAKING, MADE, REGISTERING, REGISTERED, NONE, GONE } state = QUEUED;
  std::shared_ptr<const std::filesystem::path> file;  // where it comes from
  uint32_t index = 0;                                   // its image in `file`
  Image image;                 // QUEUED: the original
  std::vector<uint8_t> texels; // MADE: the copy and its mips
  uint32_t width = 0, height = 0, mips = 0;
  uint64_t bytes = 0;  // held, made or registered
  uint64_t order = 0;  // made or registered: older ones go first
};

std::mutex g_mutex;
std::condition_variable_any g_changed;
std::unordered_map<Key, Copy, KeyHash> g_copies;  // never erased
std::deque<std::filesystem::path> g_files;  // read, images not listed yet
std::deque<Key> g_queue;                    // listed, not made yet
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_read;
unsigned g_listing = 0;  // files read whose images aren't listed yet
uint64_t g_held = 0, g_order = 0;
bool g_watching = false;
std::vector<std::jthread> g_workers;  // last: stop before the rest goes

std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

uint32_t Be32(const uint8_t* p)
{
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

// On the reading thread: the file goes to the workers once per load.
void OnRead(const std::filesystem::path& file, void*)
{
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(g_mutex);
  auto [it, added] = g_read.try_emplace(file.generic_string(), now);
  if (!added && now - it->second < SAME_LOAD)
    return;
  it->second = now;
  g_files.push_back(file);
  ++g_listing;
  g_changed.notify_all();
}

// Under the lock.
bool Missing(const Key& key)
{
  const auto it = g_copies.find(key);
  return it == g_copies.end() || it->second.state == Copy::GONE;
}

// The file's images queued by their keys: only the ones not held are decoded.
void List(const std::filesystem::path& file)
{
  const std::vector<uint8_t> bytes = ReadFile(file);
  std::vector<std::pair<Key, uint32_t>> keys;
  if (bytes.size() >= 12 && Be32(&bytes[0]) == 0x0020AF30)
  {
    const uint32_t table = Be32(&bytes[8]);
    const uint32_t count =
        table < bytes.size() ? std::min<uint32_t>(Be32(&bytes[4]), static_cast<uint32_t>((bytes.size() - table) / 8)) : 0;
    for (uint32_t i = 0; i < count; ++i)
    {
      const auto tpl = DecodeTplImage(bytes, i, false);
      if (tpl && tpl->image.width >= 8 && tpl->image.height >= 8)
        keys.emplace_back(aurora::texture::source_key(tpl->format, tpl->image.width, tpl->image.height, &bytes[tpl->offset]),
                          i);
    }
  }
  {
    std::lock_guard lock(g_mutex);
    std::erase_if(keys, [](const auto& k) { return !Missing(k.first); });
  }
  std::vector<std::pair<size_t, Image>> images;  // index into `keys`
  for (size_t i = 0; i < keys.size(); ++i)
    if (auto tpl = DecodeTplImage(bytes, keys[i].second))
      images.emplace_back(i, std::move(tpl->image));
  const auto path = std::make_shared<const std::filesystem::path>(file);
  std::lock_guard lock(g_mutex);
  for (auto& [i, image] : images)
  {
    const Key& key = keys[i].first;
    if (!Missing(key))
      continue;
    g_copies[key] = Copy{.file = path, .index = keys[i].second, .image = std::move(image)};
    g_queue.push_back(key);
  }
  --g_listing;
  g_changed.notify_all();
}

// The copy of `image`, or nothing when it would be no larger.
std::vector<uint8_t> Make(const Image& image, uint32_t& width, uint32_t& height, uint32_t& mips)
{
  uint32_t scale = MAX_SCALE;
  while (scale > 1 && static_cast<uint64_t>(image.width) * image.height * scale * scale > MAX_TEXELS)
    --scale;
  if (scale == 1 || image.rgba.empty())
    return {};
  const Image large = UpscaleImage(image, scale);
  width = large.width;
  height = large.height;
  return MipChain(large, mips);
}

// Under the lock: `copy` made (its texels in place), others let go while over
// budget, the ones never drawn first, then the longest drawn. Returns the
// registered ones let go, for aurora to drop.
std::vector<Key> Made(Copy& copy)
{
  copy.state = copy.texels.empty() ? Copy::NONE : Copy::MADE;
  copy.image = {};
  copy.bytes = copy.texels.size();
  copy.order = ++g_order;
  g_held += copy.bytes;
  std::vector<Key> dropped;
  const auto rank = [](const Copy& c) { return c.state == Copy::MADE ? 0 : 1; };
  while (g_held > BUDGET)
  {
    auto oldest = g_copies.end();
    for (auto it = g_copies.begin(); it != g_copies.end(); ++it)
    {
      const Copy& c = it->second;
      if ((c.state != Copy::MADE && c.state != Copy::REGISTERED) || &c == &copy)
        continue;
      if (oldest == g_copies.end() || rank(c) < rank(oldest->second) ||
          (rank(c) == rank(oldest->second) && c.order < oldest->second.order))
        oldest = it;
    }
    if (oldest == g_copies.end())
      break;
    Copy& gone = oldest->second;
    if (gone.state == Copy::REGISTERED)
      dropped.push_back(oldest->first);
    g_held -= gone.bytes;
    gone.state = Copy::GONE;
    gone.texels = {};
    gone.bytes = 0;
  }
  g_changed.notify_all();
  return dropped;
}

// Under the lock: `key`'s copy (QUEUED or GONE) made here and now.
void MakeHere(std::unique_lock<std::mutex>& lock, const Key& key, std::stop_token stop = {})
{
  Copy& copy = g_copies.at(key);
  const bool gone = copy.state == Copy::GONE;
  copy.state = Copy::MAKING;
  Image image = std::move(copy.image);
  const auto file = copy.file;
  const uint32_t index = copy.index;
  lock.unlock();
  if (gone)
    if (auto tpl = DecodeTplImage(ReadFile(*file), index))
      image = std::move(tpl->image);
  uint32_t width = 0, height = 0, mips = 0;
  std::vector<uint8_t> texels = Make(image, width, height, mips);
  lock.lock();
  copy.texels = std::move(texels);
  copy.width = width, copy.height = height, copy.mips = mips;
  // Dropped under the lock, for a copy made again meanwhile not to lose its
  // registration (aurora calls the provider without its own lock held). Not
  // once quitting: aurora may be gone.
  for (const Key& k : Made(copy))
    if (!stop.stop_requested())
      aurora::texture::remove_memory_replacement(k);
}

void Work(std::stop_token stop)
{
  std::unique_lock lock(g_mutex);
  while (g_changed.wait(lock, stop, [] { return !g_files.empty() || !g_queue.empty(); }) && !stop.stop_requested())
  {
    if (!g_files.empty())
    {
      const std::filesystem::path file = std::move(g_files.front());
      g_files.pop_front();
      lock.unlock();
      List(file);
      lock.lock();
      continue;
    }
    const Key key = g_queue.front();
    g_queue.pop_front();
    const auto it = g_copies.find(key);
    if (it != g_copies.end() && it->second.state == Copy::QUEUED)
      MakeHere(lock, key, stop);
  }
}
}  // namespace

void WatchUiImages(ios::Disc& disc, const std::filesystem::path& game)
{
  disc.WatchReads(game / FOLDER, OnRead, nullptr);
  const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 1u, WORKERS);
  for (unsigned w = 0; w < count; ++w)
    g_workers.emplace_back(Work);
  g_watching = true;
}

bool ProvideUiImage(const aurora::texture::TextureSource& source)
{
  if (!g_watching || source.tlut != nullptr)
    return false;
  std::unique_lock lock(g_mutex);
  // A file just read may hold it: its images are listed in a few ms (here,
  // when no worker has taken it yet).
  while (g_listing != 0)
  {
    if (g_files.empty())
    {
      g_changed.wait(lock, [] { return g_listing == 0 || !g_files.empty(); });
      continue;
    }
    const std::filesystem::path file = std::move(g_files.front());
    g_files.pop_front();
    lock.unlock();
    List(file);
    lock.lock();
  }
  const auto it = g_copies.find(source.key);
  if (it == g_copies.end())
    return false;
  // References into g_copies stay valid: entries are never erased.
  Copy& copy = it->second;
  g_changed.wait(lock, [&] { return copy.state != Copy::MAKING; });
  if (copy.state == Copy::QUEUED || copy.state == Copy::GONE)
    MakeHere(lock, source.key);  // not begun or let go: made here, now
  if (copy.state != Copy::MADE)
    return copy.state == Copy::REGISTERED;
  // Registered outside the lock (aurora takes its own); REGISTERING keeps it
  // from being let go meanwhile.
  copy.state = Copy::REGISTERING;
  const std::vector<uint8_t> texels = std::move(copy.texels);
  copy.texels = {};
  const uint32_t width = copy.width, height = copy.height, mips = copy.mips;
  lock.unlock();
  aurora::texture::set_memory_replacement(source.key, width, height, mips, texels);
  lock.lock();
  copy.state = Copy::REGISTERED;
  copy.order = ++g_order;
  return true;
}
}  // namespace dualize::nights
