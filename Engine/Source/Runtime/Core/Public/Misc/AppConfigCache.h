#pragma once

#include "CoreAPI.h"

namespace Durin
{
	class FYamlNodeView
	{
	public:
		FYamlNodeView() = default;
		FYamlNodeView(void* InTreePtr, size_t InNodeIndex)
			: TreePtr(InTreePtr)
			, NodeIndex(InNodeIndex)
		{
		}
		~FYamlNodeView() = default;

		CORE_API auto GetStringValue(std::string_view InKey, std::string DefaultValue = "") const -> std::string;

		CORE_API auto GetBoolValue(std::string_view InKey, bool DefaultValue = false) const -> bool;

		CORE_API auto GetFloatValue(std::string_view InKey, float DefaultValue = 0.0f) const -> float;

		CORE_API auto GetIntValue(std::string_view InKey, int DefaultValue = 0) const -> int;

		CORE_API auto GetView(std::string_view InKey) const -> FYamlNodeView;

	private:
		void* TreePtr = nullptr;

		size_t NodeIndex = 0;
	};

	CORE_API extern FYamlNodeView GAppConfig;

	CORE_API auto IsAppConfigLoaded() -> bool;

	namespace CoreInternal
	{
		CORE_API auto LoadApplicationConfig(const std::string& ConfigFile) -> bool;
	}
} // namespace Durin