#include "resident_texture_run.h"
#include <type_traits>

static_assert(!std::is_copy_constructible_v<ResidentTextureRun>);
static_assert(!std::is_copy_constructible_v<GpuResidentRun>);
static_assert(!std::is_copy_constructible_v<FiniteFields>);
static_assert(!std::is_move_constructible_v<TextureHandoff>);