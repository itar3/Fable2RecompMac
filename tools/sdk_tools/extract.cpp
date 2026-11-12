// Extracts an Xbox 360 game with the ReXGlue SDK's filesystem devices: a
// Games on Demand (STFS/SVOD) package by its header file, or a disc image
// (.iso, XDVDFS) by the image file.
//
// Usage: extract [--only=<name>] [--progress] <package header or .iso> <out dir>
//   --only=<name>  extract only the entries with that name (e.g. default.xex)
//   --progress     print "progress <done bytes> <total bytes>" lines (for the launcher)
//        extract --image-hash <game dir>
//   loads <game dir>/default.xex as the game does (applying default.xexp when
//   present) and prints "image <XXH3-128>" of the guest image: the program the
//   recompiled code was made from has one fixed value (the launcher checks it).
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rex::filesystem;

static size_t g_files = 0, g_bytes = 0, g_errors = 0, g_total = 0, g_reported = 0;
static std::string g_only;
static bool g_progress = false;

static bool Wanted(Entry* e) { return g_only.empty() || e->name() == g_only; }

static size_t TotalBytes(Entry* e) {
  size_t total = 0;
  for (auto& c : e->children())
    total += (c->attributes() & kFileAttributeDirectory) ? TotalBytes(c.get()) : (Wanted(c.get()) ? c->size() : 0);
  return total;
}

static void Report(bool force = false) {
  if (g_progress && (force || g_bytes - g_reported >= (64u << 20))) {
    g_reported = g_bytes;
    std::printf("progress %zu %zu\n", g_bytes, g_total);
    std::fflush(stdout);
  }
}

static void Dump(Entry* e, const fs::path& out) {
  for (auto& c : e->children()) {
    fs::path p = out / c->name();
    if (c->attributes() & kFileAttributeDirectory) {
      if (g_only.empty())
        fs::create_directories(p);
      Dump(c.get(), g_only.empty() ? p : out);
      continue;
    }
    if (!Wanted(c.get()))
      continue;
    File* f = nullptr;
    if (c->Open(FileAccess::kFileReadData, &f) != 0 || !f) {
      std::fprintf(stderr, "open failed: %s\n", c->path().c_str());
      ++g_errors;
      continue;
    }
    std::ofstream os(p, std::ios::binary);
    std::vector<uint8_t> buf(8 << 20);
    size_t off = 0, total = c->size();
    while (off < total) {
      size_t want = std::min(buf.size(), total - off), got = 0;
      if (f->ReadSync({buf.data(), want}, off, &got) != 0 || got == 0) {
        std::fprintf(stderr, "read failed: %s @%zu\n", c->path().c_str(), off);
        ++g_errors;
        break;
      }
      os.write(reinterpret_cast<char*>(buf.data()), got);
      off += got;
      g_bytes += got;
      Report();
    }
    f->Destroy();
    ++g_files;
  }
}

// The guest image of Fable 2's default.xex (load address and size of every
// version seen: base 0x82000000, 0x1620000 bytes).
constexpr uint32_t kImageBase = 0x82000000, kImageSize = 0x1620000;

static int ImageHash(const char* game_dir) {
  rex::Runtime rt(game_dir, game_dir);
  rex::RuntimeConfig cfg;
  cfg.tool_mode = true;
  cfg.kernel_init = rex::kernel::InitializeKernel;
  if (XFAILED(rt.Setup(std::move(cfg)))) {
    std::fprintf(stderr, "runtime setup failed\n");
    return 1;
  }
  if (XFAILED(rt.LoadXexImage("game:\\default.xex"))) {
    std::fprintf(stderr, "the game program could not be loaded (or its update did not apply)\n");
    return 1;
  }
  const auto* image = rt.memory()->TranslateVirtual<const uint8_t*>(kImageBase);
  const XXH128_hash_t h = XXH3_128bits(image, kImageSize);
  std::printf("image %016llx%016llx\n", static_cast<unsigned long long>(h.high64),
              static_cast<unsigned long long>(h.low64));
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--image-hash")
    return ImageHash(argv[2]);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--only=", 0) == 0)
      g_only = a.substr(7);
    else if (a == "--progress")
      g_progress = true;
    else
      args.push_back(a);
  }
  if (args.size() != 2) {
    std::fprintf(stderr, "usage: %s [--only=<name>] [--progress] <package header or .iso> <out dir>\n", argv[0]);
    return 2;
  }
  const fs::path in = args[0];
  std::string ext = in.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return std::tolower(ch); });
  std::unique_ptr<Device> dev;
  if (ext == ".iso")
    dev = std::make_unique<DiscImageDevice>("\\disc", in);
  else
    dev = std::make_unique<StfsContainerDevice>("\\pkg", in);
  if (!dev->Initialize()) {
    std::fprintf(stderr, "failed to open %s\n", ext == ".iso" ? "the disc image" : "the package");
    return 1;
  }
  Entry* root = dev->ResolvePath("");
  if (!root) {
    std::fprintf(stderr, "no root entry\n");
    return 1;
  }
  g_total = TotalBytes(root);
  fs::create_directories(args[1]);
  Report(true);
  Dump(root, args[1]);
  Report(true);
  std::printf("extracted %zu files, %.2f GB, %zu errors\n", g_files, g_bytes / 1e9, g_errors);
  return g_errors ? 1 : 0;
}
