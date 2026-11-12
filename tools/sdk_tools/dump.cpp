// Loads a XEX (applying default.xexp if present) with the ReXGlue runtime in
// tool mode and writes the guest image range to a file.
// Usage: dump <game dir> <image base hex> <image size hex> <out file>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/kernel/init.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  uint32_t base = std::strtoul(argv[2], nullptr, 16), size = std::strtoul(argv[3], nullptr, 16);
  rex::Runtime rt(argv[1], argv[1]);
  rex::RuntimeConfig cfg;
  cfg.tool_mode = true;
  cfg.kernel_init = rex::kernel::InitializeKernel;
  if (XFAILED(rt.Setup(std::move(cfg)))) { std::fprintf(stderr, "setup failed\n"); return 1; }
  if (XFAILED(rt.LoadXexImage("game:\\default.xex"))) { std::fprintf(stderr, "load failed\n"); return 1; }
  auto* p = rt.memory()->TranslateVirtual<const char*>(base);
  std::ofstream(argv[4], std::ios::binary).write(p, size);
  std::printf("dumped 0x%08X+0x%X\n", base, size);
  return 0;
}
