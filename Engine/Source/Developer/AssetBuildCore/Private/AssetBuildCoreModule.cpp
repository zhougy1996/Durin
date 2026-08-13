#include "AssetBuild/AssetBuildCoreModule.h"
#include "AssetBuild/BuildHost.h"

namespace Durin
{
	class FAssetBuildCoreModule final : public Asset::Build::IAssetBuildCoreModule
	{
	public:
		auto InitializeHost(std::string* OutError) -> bool override
		{
			return Asset::Build::InitializeBuildHost(OutError);
		}

		auto ShutdownHost() -> void override
		{
			Asset::Build::ShutdownBuildHost();
		}
	};

	IMPLEMENT_MODULE(FAssetBuildCoreModule, AssetBuildCore)
}
