#include "Misc/AppConfigCache.h"

#include "Misc/FileHelper.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

namespace Doge
{
	auto FConfigView::GetString(const std::string& Name) const -> std::string
	{
		std::string Value;
		ryml::NodeRef Node = ryml::NodeRef{static_cast<ryml::Tree*>(TreePtr), NodeIndex};
		Node[Name.c_str()] >> Value;
		return Value;
	}

	FConfigView GAppConfig;

	static std::string AppConfigContent{};

	static std::unique_ptr<ryml::Tree> AppConfigTree;

	namespace CoreInternal
	{
		auto LoadApplicationConfig(const std::string& ConfigFile) -> bool
		{
			check(!AppConfigTree); // Ensure this function is only called once.

			AppConfigTree = std::make_unique<ryml::Tree>();

			bool bLoadSuccess = FFileHelper::LoadFileToString(AppConfigContent, ConfigFile);
			if (!bLoadSuccess)
			{
				DOGE_ERROR("Failed to load application config file: {}", ConfigFile);
				return false;
			}

			ryml::parse_in_place(AppConfigContent.data(), AppConfigTree.get());
			GAppConfig = {AppConfigTree.get(), AppConfigTree->root_id()};

			return true;
		}
	} // namespace CoreInternal

	auto IsAppConfigLoaded() -> bool
	{
		return AppConfigTree != nullptr;
	}


} // namespace Doge
