// Advances every AnimatorComponent: samples its clip (with crossfades) on
// the skeleton of the actor's skinned meshes and stores the model-space
// joint matrices the renderer skins with.
#pragma once

#include "Hollow/Scene/Scene.hpp"

namespace Hollow {

// `playing`: Play mode. Outside it, animators with preview_in_editor still
// advance (so imported animations can be checked in the editor).
void update_animators(Scene& scene, float dt, bool playing);

} // namespace Hollow
