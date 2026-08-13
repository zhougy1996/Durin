#pragma once

#include "AssetImportCore.h"

namespace Durin::Asset::Import
{
	auto CreateSceneImportProvider()
		-> std::shared_ptr<IImportProvider>;
}
