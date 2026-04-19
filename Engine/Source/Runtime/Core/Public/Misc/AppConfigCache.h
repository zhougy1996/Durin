#pragma once

namespace Doge
{
	class FConfigView
	{
	public:
		FConfigView() = default;
		FConfigView(void* InTreePtr, size_t InNodeIndex)
			: TreePtr(InTreePtr)
			, NodeIndex(InNodeIndex)
		{
		}
		~FConfigView() = default;

		CORE_API auto GetString(const std::string& Name) const -> std::string;

	private:
		void* TreePtr = nullptr;
		size_t NodeIndex = 0;
	};

	CORE_API extern FConfigView GAppConfig;

	CORE_API auto IsAppConfigLoaded() -> bool;

	namespace CoreInternal
	{
		CORE_API auto LoadApplicationConfig(const std::string& ConfigFile) -> bool;
	}
} // namespace Doge