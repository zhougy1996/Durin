#pragma once

#include "ImportedSceneInternal.h"

#include "Json/Json.h"

namespace Durin::AssetForge::Builtins::Private
{
	auto ImportGltfSkeletalData(
		FJsonNodeView Root,
		const std::vector<FByteArray>& Buffers,
		FSceneDecodeResult& Result) -> bool;
}
