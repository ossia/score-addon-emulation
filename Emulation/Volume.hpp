#pragma once

namespace Emulation
{
// Linear post-render gain. Unity keeps its existing meaning; boost allows
// quiet emulation cores and native presets to use the same output level.
inline constexpr float MaxVolume = 32.f;
}
