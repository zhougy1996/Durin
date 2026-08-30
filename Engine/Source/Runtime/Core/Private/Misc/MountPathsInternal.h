#pragma once

#include "Misc/MountPaths.h"

namespace Durin::MountPathInternal
{
	auto MutableMountPoints() -> std::vector<FMountPoint>&;
	auto RegistryPublished() -> bool&;
}
