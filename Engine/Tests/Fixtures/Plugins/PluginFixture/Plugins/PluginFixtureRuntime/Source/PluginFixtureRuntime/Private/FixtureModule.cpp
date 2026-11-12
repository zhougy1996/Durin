#include "Modules/ModuleManager.h"
namespace Durin
{
	class FPluginFixtureRuntimeModule final : public IModuleInterface
	{
		auto Record(std::string_view Event) -> void
		{
			if (const char* Path = std::getenv("DURIN_PLUGIN_FIXTURE_TRACE"))
			{
				std::ofstream Stream(Path, std::ios::app);
				Stream << "PluginFixtureRuntime " << Event << "\n";
				if (!Stream) throw std::runtime_error("Cannot write plugin fixture trace");
			}
		}
	public:
		auto StartupModule() -> void override
		{
			Record("start");
			if (const char* Fail = std::getenv("DURIN_PLUGIN_FIXTURE_FAIL");
				Fail && std::string_view(Fail) == "PluginFixtureRuntime")
				throw std::runtime_error("Requested fixture provider failure");
		}
		auto ShutdownModule() -> void override { Record("stop"); }
		auto SupportsDynamicReloading() const -> bool override { return true; }
	};
	IMPLEMENT_MODULE(FPluginFixtureRuntimeModule, PluginFixtureRuntime)
}
