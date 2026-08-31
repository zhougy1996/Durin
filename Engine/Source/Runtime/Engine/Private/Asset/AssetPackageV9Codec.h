#pragma once

#include "AssetPackageCodec.h"

namespace Durin::Asset::Private::DastV9
{
	ENGINE_API auto GetCodec() -> const FAssetPackageCodec&;
}
