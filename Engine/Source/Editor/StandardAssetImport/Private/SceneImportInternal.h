#pragma once

#include "AssetImportCore.h"

namespace Durin::Asset::Import::Standard
{
	auto CreateSceneImportProvider()
		-> std::shared_ptr<IImportProvider>;
}
