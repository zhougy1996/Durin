#pragma once

#include "ImportedSceneInternal.h"

#include "Json/Json.h"

namespace Durin::Asset::Forge::Private
{
	auto ImportGltfSkeletalData(
		FJsonNodeView Root,
		const std::vector<std::vector<uint8>>& Buffers,
		FSceneDecodeResult& Result) -> bool;
}
