#include "Modules/ModuleManager.h"
#include "Misc/AppConfig.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCookedLibrary.h"
namespace Durin
{
    // A native-only program: no material asset can retain this declaration.
    static FShaderType FixtureShader("PluginFixtureVertex", "/Plugins/PluginFixtureRuntime/Fixture",
        EShaderFrequency::Vertex, "VertexMain");
    static FShaderProgramRegistration FixtureProgram("PluginFixtureRuntime", "PluginFixtureProgram",
        EShaderRequestEligibility::GameAndEditor, {&FixtureShader});
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
            if (const char* Expected = std::getenv("DURIN_PLUGIN_FIXTURE_EXPECT_CONFIG"))
            {
                const auto Config = GetModuleConfig("PluginFixture");
                if (Config.GetView("Default").GetString() != "PluginFixtureConsumer"
                    || !Config.GetView("ProjectWins").GetBool()
                    || Config.GetView("UserWins").GetString() != Expected)
                    throw std::runtime_error("Plugin configuration precedence was not applied before provider startup");
            }
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
