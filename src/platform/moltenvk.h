// platform/moltenvk.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#if defined(__APPLE__)

#include <plume_render_interface_types.h>

namespace eot::platform {

bool PrepareMoltenVK();

bool GetMetalRenderWindow(plume::RenderWindow &out);

}

#endif
