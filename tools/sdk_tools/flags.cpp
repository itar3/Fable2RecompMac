// Prints every registered cvar (runtime + xenos GPU plugin) with its default.
#include <rex/cvar.h>
#include <rex/system/gpu_plugin.h>
#include <cstdio>
int main() {
  auto gfx = rex::system::LoadGpuPlugin("xenos");
  for (const auto& f : rex::cvar::GetRegistry())
    std::printf("%-12s %-48s = %-12s  %s\n", f.category.c_str(), f.name.c_str(),
                f.default_value.c_str(), f.description.c_str());
  return gfx ? 0 : 1;
}
