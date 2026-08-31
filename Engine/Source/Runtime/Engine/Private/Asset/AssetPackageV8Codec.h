#pragma once

#include "AssetPackageCodec.h"

namespace Durin::Asset::Private::DastV8
{
	ENGINE_API auto GetCodec() -> const FAssetPackageCodec&;
}
