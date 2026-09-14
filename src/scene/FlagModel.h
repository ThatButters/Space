#pragma once
#include "gfx/GltfModel.h"

namespace space {

// The Apollo Lunar Flag Assembly: a 3 x 5 ft nylon United States flag on an aluminium mast with a
// telescoping crossbar along the top edge (so it stays spread in vacuum). Geometry in metres, +Y up,
// origin at the foot of the mast; the flag hangs toward +X. The cloth carries the permanent ripple
// the Apollo 11 crossbar left when it did not fully extend. Texture is generated (50 stars, 13 stripes).
gfx::ModelData makeApolloFlagModel();

} // namespace space
