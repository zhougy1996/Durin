#pragma once

#include "CoreMinimal.h"
#include "DObject/CookLifecycle.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
{
	inline auto AppendCookSchemaVersion(FObjectCookContext& Context, std::string_view Name, uint32 Version) -> void
	{
		FBinaryWriter Bytes;
		Bytes.WriteU32(Version);
		Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion,
			"class/" + std::string(Name), {}, Bytes.TakeBytes()});
	}
}
