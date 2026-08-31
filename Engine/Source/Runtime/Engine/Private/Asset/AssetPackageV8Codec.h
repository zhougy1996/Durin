#pragma once

#include "AssetPackageCodec.h"

namespace Durin::Asset::Private::DastV8
{
	auto GetCodec() -> const FAssetPackageCodec&;
}
