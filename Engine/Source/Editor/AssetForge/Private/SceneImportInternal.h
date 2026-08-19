#pragma once

#include "AssetImportCore.h"

namespace Durin::Asset::Forge
{
	auto CreateSceneImportProvider()
		-> std::shared_ptr<IImportProvider>;
}
