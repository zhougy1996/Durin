#pragma once

#include "AssetPackageCodec.h"

namespace Durin::AssetPrivate::DastV9
{
	ENGINE_API auto GetCodec() -> const FAssetPackageCodec&;
	ENGINE_API auto GetV10Codec() -> const FAssetPackageCodec&;
}
