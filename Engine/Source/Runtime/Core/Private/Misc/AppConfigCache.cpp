#include "Misc/AppConfigCache.h"

#include "Misc/FileHelper.h"

#include <ryml.hpp>
#include <ryml_std.hpp>

namespace Durin
{
	static FORCEINLINE auto MakeRymlNodeRef(void* TreePtr, size_t NodeIndex) -> ryml::NodeRef
	{
		return ryml::NodeRef{static_cast<ryml::Tree*>(TreePtr), NodeIndex};
	}

	auto FYamlNodeView::GetStringValue(std::string_view InKey, std::string DefaultValue) const -> std::string
	{
		std::string Value;
		auto Node = MakeRymlNodeRef(TreePtr, NodeIndex)[InKey.data()];
		if (Node.readable())
		{
			Node >> Value;
		}
		return Value;
	}

	auto FYamlNodeView::GetBoolValue(std::string_view InKey, bool DefaultValue) const -> bool
	{
		bool Value = DefaultValue;
		auto Node = MakeRymlNodeRef(TreePtr, NodeIndex)[InKey.data()];
		if (Node.readable())
		{
			Node >> Value;
		}
		return Value;
	}

	auto FYamlNodeView::GetFloatValue(std::string_view InKey, float DefaultValue) const -> float
	{
		float Value = 0.f;
		auto Node = MakeRymlNodeRef(TreePtr, NodeIndex)[InKey.data()];
		if (Node.readable())
		{
			Node >> Value;
		}
		return Value;
	}

	auto FYamlNodeView::GetIntValue(std::string_view InKey, int DefaultValue) const -> int
	{
		int Value = DefaultValue;
		auto Node = MakeRymlNodeRef(TreePtr, NodeIndex)[InKey.data()];
		if (Node.readable())
		{
			Node >> Value;
		}
		return Value;
	}

	auto FYamlNodeView::GetView(std::string_view InKey) const -> FYamlNodeView
	{
		const auto Node = MakeRymlNodeRef(TreePtr, NodeIndex)[InKey.data()];
		return FYamlNodeView{TreePtr, Node.id()};
	}

	FYamlNodeView GAppConfig;

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
				DURIN_ERROR("Failed to load application config file: {}", ConfigFile);
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
