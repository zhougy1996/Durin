#include "AssetBuild/AssetBuildCoreModule.h"

namespace Durin
{
	class FAssetBuildCoreModule final : public Asset::Build::IAssetBuildCoreModule
	{
	};

	IMPLEMENT_MODULE(FAssetBuildCoreModule, AssetBuildCore)
}
