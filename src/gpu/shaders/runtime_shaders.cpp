// gpu/shaders/runtime_shaders.cpp
// Fable 2: shaders the built-in cache lacks, translated while the game runs
// (see runtime_shaders.h).

#include "gpu/shaders/runtime_shaders.h"

#include <fcntl.h>
#include <spawn.h>
#include <pthread/spawn.h>
#include <sys/qos.h>
#include <sys/wait.h>

#include <algorithm>
#include <cerrno>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include "core/logging.h"

extern char **environ;

REXCVAR_DEFINE_BOOL(fable2_runtime_shaders, true, "Fable2",
                    "Native renderer: translate shaders missing from the built-in cache while the game runs "
                    "(their draws were dropped: missing or black geometry in areas not captured before the build)");
REXCVAR_DEFINE_STRING(fable2_runtime_shader_dir, "", "Fable2",
                      "Native renderer: where translated shaders are kept between runs (default: "
                      "shader_runtime next to the executable)");
REXCVAR_DEFINE_STRING(fable2_xenosrecomp, "", "Fable2",
                      "Native renderer: the XenosRecomp executable for runtime shader translation (default: the "
                      "one this build made, or XenosRecomp next to the executable)");
REXCVAR_DEFINE_STRING(fable2_shader_common, "", "Fable2",
                      "Native renderer: shader_common.h for runtime shader translation (default: the one this "
                      "build used, or shader_common.h next to the executable)");
REXCVAR_DEFINE_INT32(fable2_runtime_shader_workers, 3, "Fable2",
                     "Native renderer: background threads translating missing shaders");

namespace eot::gpu {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMagic = 0x43533246u;  // 'F2SC'
constexpr uint32_t kVersion = 1;
constexpr size_t kEntryBytes = 8 + 24 * 4;

struct Blob {
  std::vector<uint8_t> spirv;
  std::vector<uint32_t> layouts;
  std::vector<ShaderCacheEntry> entries;
};

struct Job {
  uint64_t hash = 0;
  std::vector<uint8_t> container;
};

struct State {
  std::mutex mutex;
  std::deque<std::unique_ptr<Blob>> blobs;
  std::unordered_map<uint64_t, const ShaderCacheEntry *> by_hash;
  std::unordered_map<const ShaderCacheEntry *, const Blob *> by_entry;
  std::unordered_set<uint64_t> requested;  // queued, done or failed this session
  std::deque<Job> queue;
  std::condition_variable wake;
  std::vector<std::thread> workers;
  fs::path dir;
  bool initialized = false;
};

State &state() {
  static State s;
  return s;
}

fs::path FirstExisting(const std::string &override_path, const char *built, const char *beside_exe) {
  if (!override_path.empty())
    return override_path;
  std::error_code ec;
  if (built && *built && fs::exists(built, ec))
    return built;
  return rex::filesystem::GetExecutableFolder() / beside_exe;
}

fs::path TranslatorPath() {
#ifdef FABLE2_XENOSRECOMP_EXE
  return FirstExisting(REXCVAR_GET(fable2_xenosrecomp), FABLE2_XENOSRECOMP_EXE, "XenosRecomp");
#else
  return FirstExisting(REXCVAR_GET(fable2_xenosrecomp), nullptr, "XenosRecomp");
#endif
}

fs::path ShaderCommonPath() {
#ifdef FABLE2_SHADER_COMMON_H
  return FirstExisting(REXCVAR_GET(fable2_shader_common), FABLE2_SHADER_COMMON_H, "shader_common.h");
#else
  return FirstExisting(REXCVAR_GET(fable2_shader_common), nullptr, "shader_common.h");
#endif
}

uint32_t Read32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// Parses a --blob file into a Blob; false if it is malformed. Empty (0 entries)
// means XenosRecomp could not translate the shader.
bool ParseBlob(const std::vector<uint8_t> &b, Blob &out) {
  if (b.size() < 20 || Read32(b.data()) != kMagic || Read32(b.data() + 4) != kVersion)
    return false;
  const uint32_t count = Read32(b.data() + 8), layout_words = Read32(b.data() + 12),
                 spirv_bytes = Read32(b.data() + 16);
  const size_t need = 20 + size_t(count) * kEntryBytes + size_t(layout_words) * 4 + spirv_bytes;
  if (count > 4096 || b.size() != need)
    return false;
  const uint8_t *p = b.data() + 20;
  out.entries.resize(count);
  for (uint32_t i = 0; i < count; ++i, p += kEntryBytes) {
    ShaderCacheEntry &e = out.entries[i];
    std::memcpy(&e.hash, p, 8);
    uint32_t f[24];
    std::memcpy(f, p + 8, sizeof(f));
    e.dxilOffset = f[0];
    e.dxilSize = f[1];
    e.spirvOffset = f[2];
    e.spirvSize = f[3];
    e.specConstantsMask = f[4];
    e.vertexLayoutOffset = f[5];
    e.vertexLayoutCount = f[6];
    e.vfetchCodeOffset = f[7];
    e.usesFloatConstants = f[8];
    e.interpolantMask = f[9];
    e.fullDxilOffset = f[10];
    e.fullDxilSize = f[11];
    e.fullSpirvOffset = f[12];
    e.fullSpirvSize = f[13];
    e.posDxilOffset = f[14];
    e.posDxilSize = f[15];
    e.posSpirvOffset = f[16];
    e.posSpirvSize = f[17];
    e.velDxilOffset = f[18];
    e.velDxilSize = f[19];
    e.velSpirvOffset = f[20];
    e.velSpirvSize = f[21];
    e.textureFetchMask = f[22];
    e.shaderFlags = f[23];
    auto in_spirv = [&](uint32_t off, uint32_t size) { return size == 0 || (off <= spirv_bytes && size <= spirv_bytes - off); };
    if (!in_spirv(e.spirvOffset, e.spirvSize) || !in_spirv(e.fullSpirvOffset, e.fullSpirvSize) ||
        !in_spirv(e.posSpirvOffset, e.posSpirvSize) || !in_spirv(e.velSpirvOffset, e.velSpirvSize) ||
        e.spirvSize == 0 || uint64_t(e.vertexLayoutOffset) + uint64_t(e.vertexLayoutCount) * 2 > layout_words)
      return false;
  }
  out.layouts.resize(layout_words);
  std::memcpy(out.layouts.data(), p, size_t(layout_words) * 4);
  p += size_t(layout_words) * 4;
  out.spirv.assign(p, p + spirv_bytes);
  return true;
}

std::vector<uint8_t> ReadFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Adds a parsed blob's entries to the table; returns how many were new.
size_t Publish(std::unique_ptr<Blob> blob) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  size_t added = 0;
  for (const ShaderCacheEntry &e : blob->entries)
    if (s.by_hash.emplace(e.hash, &e).second) {
      s.by_entry.emplace(&e, blob.get());
      s.requested.insert(e.hash);
      ++added;
    }
  s.blobs.push_back(std::move(blob));
  return added;
}

bool RunTranslator(const fs::path &in, const fs::path &out, std::string *error) {
  const std::string exe = TranslatorPath().string(), common = ShaderCommonPath().string();
  const std::string in_s = in.string(), out_s = out.string();
  std::vector<char *> argv = {const_cast<char *>(exe.c_str()), const_cast<char *>(in_s.c_str()),
                              const_cast<char *>(out_s.c_str()), const_cast<char *>(common.c_str()),
                              const_cast<char *>("--blob"), nullptr};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  // The translator would inherit the calling game thread's QoS, which can be
  // background: macOS then keeps it on the efficiency cores, throttled (a
  // translation took 36 s instead of 5 s, and on a first start with no built-in
  // cache the queue fell minutes behind: characters and ground stayed missing).
  // Utility runs it at full speed while still yielding to the game's threads.
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_set_qos_class_np(&attr, QOS_CLASS_UTILITY);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    *error = std::format("cannot start {} ({})", exe, std::strerror(rc));
    return false;
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    *error = std::format("{} exited with {}", exe, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return false;
  }
  return true;
}

void Translate(const Job &job) {
  auto &s = state();
  const auto start = std::chrono::steady_clock::now();
  const std::string name = std::format("{:016x}", job.hash);
  std::error_code ec;
  const fs::path in = s.dir / "in" / (name + ".bin");
  const fs::path tmp = s.dir / (name + ".f2sc.tmp");
  const fs::path out = s.dir / (name + ".f2sc");
  {
    std::ofstream f(in, std::ios::binary);
    f.write(reinterpret_cast<const char *>(job.container.data()), std::streamsize(job.container.size()));
  }
  std::string error;
  if (!RunTranslator(in, tmp, &error)) {
    EOT_WARN("[shaders] runtime translation of {} failed: {}", name, error);
    return;
  }
  auto blob = std::make_unique<Blob>();
  if (!ParseBlob(ReadFile(tmp), *blob)) {
    EOT_WARN("[shaders] runtime translation of {}: unreadable output", name);
    fs::remove(tmp, ec);
    return;
  }
  if (blob->entries.empty()) {
    EOT_WARN("[shaders] runtime translation of {}: XenosRecomp could not translate it (its draws stay dropped)",
             name);
    fs::remove(tmp, ec);
    return;
  }
  fs::rename(tmp, out, ec);
  fs::remove(in, ec);
  Publish(std::move(blob));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  EOT_INFO("[shaders] runtime: {} translated in {:.0f} ms (kept in {})", name, ms, s.dir.string());
}

void Worker() {
  auto &s = state();
  for (;;) {
    Job job;
    {
      std::unique_lock lock(s.mutex);
      s.wake.wait(lock, [&] { return !s.queue.empty(); });
      job = std::move(s.queue.front());
      s.queue.pop_front();
    }
    Translate(job);
  }
}

}  // namespace

void RuntimeShadersInit() {
  auto &s = state();
  {
    std::lock_guard lock(s.mutex);
    if (s.initialized)
      return;
    s.initialized = true;
    const std::string &dir = REXCVAR_GET(fable2_runtime_shader_dir);
    s.dir = dir.empty() ? rex::filesystem::GetExecutableFolder() / "shader_runtime" : fs::path(dir);
  }
  if (!REXCVAR_GET(fable2_runtime_shaders))
    return;
  std::error_code ec;
  fs::create_directories(s.dir / "in", ec);
  {
    // Translations from another shader_common.h or translator are stale (the
    // helpers' interface with the renderer changes): drop them.
    const std::vector<uint8_t> common = ReadFile(ShaderCommonPath());
    uint64_t h = 1469598103934665603ull;
    for (const uint8_t c : common)
      h = (h ^ c) * 1099511628211ull;
    const auto translator_time = fs::last_write_time(TranslatorPath(), ec);
    const std::string stamp =
        std::format("{:016x} {}", h, ec ? 0ll : static_cast<long long>(translator_time.time_since_epoch().count()));
    ec.clear();
    const fs::path stamp_path = s.dir / "stamp.txt";
    const std::vector<uint8_t> old_stamp = ReadFile(stamp_path);
    if (std::string(old_stamp.begin(), old_stamp.end()) != stamp) {
      size_t dropped = 0;
      for (const auto &item : fs::directory_iterator(s.dir, ec))
        if (item.path().extension() == ".f2sc" && fs::remove(item.path(), ec))
          ++dropped;
      std::ofstream(stamp_path, std::ios::binary | std::ios::trunc) << stamp;
      EOT_INFO("[shaders] runtime: shader_common.h or translator changed; dropped {} earlier translations",
               dropped);
    }
  }
  size_t files = 0, entries = 0;
  for (const auto &item : fs::directory_iterator(s.dir, ec)) {
    if (item.path().extension() != ".f2sc")
      continue;
    auto blob = std::make_unique<Blob>();
    if (!ParseBlob(ReadFile(item.path()), *blob) || blob->entries.empty()) {
      fs::remove(item.path(), ec);
      continue;
    }
    ++files;
    entries += Publish(std::move(blob));
  }
  EOT_INFO("[shaders] runtime: {} shaders from earlier runs ({} files) in {}; translator {}", entries, files,
           s.dir.string(), TranslatorPath().string());
}

void RuntimeShaderRequest(uint64_t hash, const uint8_t *virtual_bytes, uint32_t virtual_size,
                          const uint8_t *physical_bytes, uint32_t physical_size) {
  if (!REXCVAR_GET(fable2_runtime_shaders) || !virtual_bytes || !virtual_size)
    return;
  RuntimeShadersInit();
  auto &s = state();
  std::lock_guard lock(s.mutex);
  if (!s.requested.insert(hash).second)
    return;
  Job job;
  job.hash = hash;
  job.container.assign(virtual_bytes, virtual_bytes + virtual_size);
  if (physical_bytes && physical_size)
    job.container.insert(job.container.end(), physical_bytes, physical_bytes + physical_size);
  s.queue.push_back(std::move(job));
  if (s.workers.empty()) {
    const int n = std::max(1, std::min(8, int(REXCVAR_GET(fable2_runtime_shader_workers))));
    for (int i = 0; i < n; ++i)
      s.workers.emplace_back(Worker).detach();
  }
  s.wake.notify_one();
  EOT_INFO("[shaders] runtime: {:016x} is not in the built-in cache, translating it", hash);
}

const ShaderCacheEntry *RuntimeShaderEntry(uint64_t hash) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.by_hash.find(hash);
  return it == s.by_hash.end() ? nullptr : it->second;
}

const uint8_t *RuntimeShaderBlob(const ShaderCacheEntry *e) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.by_entry.find(e);
  return it == s.by_entry.end() ? nullptr : it->second->spirv.data();
}

const uint32_t *RuntimeShaderLayouts(const ShaderCacheEntry *e) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.by_entry.find(e);
  return it == s.by_entry.end() ? nullptr : it->second->layouts.data();
}

}  // namespace eot::gpu
