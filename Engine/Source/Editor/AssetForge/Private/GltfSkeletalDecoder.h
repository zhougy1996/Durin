#pragma once

#include "ImportedSceneInternal.h"

#include "Json/Json.h"

namespace Durin::Asset::Forge::Private
{
	auto ImportGltfSkeletalData(
		FJsonNodeView Root,
		const std::vector<std::vector<std::byte>>& Buffers,
		FSceneDecodeResult& Result) -> bool;
}
