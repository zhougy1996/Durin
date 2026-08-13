#include "AssetBuild/AssetBuildCoreModule.h"
#include "AssetBuild/BuildHost.h"

namespace Durin
{
	class FAssetBuildCoreModule final : public AssetBuild::IAssetBuildCoreModule
	{
	public:
		auto InitializeHost(std::string* OutError) -> bool override
		{
			return AssetBuild::InitializeBuildHost(OutError);
		}

		auto ShutdownHost() -> void override
		{
			AssetBuild::ShutdownBuildHost();
		}
	};

	IMPLEMENT_MODULE(FAssetBuildCoreModule, AssetBuildCore)
}
