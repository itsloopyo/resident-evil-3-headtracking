#pragma once

#include <cameraunlock/reframework/re_math.h>

namespace RE3HT {

using cameraunlock::reframework::kDegToRad;
using cameraunlock::reframework::Matrix4x4f;
using cameraunlock::reframework::REQuat;
using cameraunlock::reframework::MatrixToQuat;
using cameraunlock::reframework::ApplyWorldSpaceHeadRotation;
using cameraunlock::reframework::ApplyCameraLocalHeadRotation;
using cameraunlock::reframework::ApplyViewSpacePositionOffset;
using cameraunlock::reframework::ProjectAimToViewTangents;

} // namespace RE3HT
