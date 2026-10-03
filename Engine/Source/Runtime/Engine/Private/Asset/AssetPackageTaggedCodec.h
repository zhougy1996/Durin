#pragma once

#include "CoreMinimal.h"

#include "AssetPackageCodec.h"

namespace Durin::AssetPrivate::TaggedPackage
{
	ENGINE_API auto GetCodec() -> const FAssetPackageCodec&;
}
