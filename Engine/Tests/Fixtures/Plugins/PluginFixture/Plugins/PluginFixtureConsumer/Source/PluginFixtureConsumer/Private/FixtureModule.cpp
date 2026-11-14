#include "Modules/ModuleManager.h"
#include "Misc/UnifiedError.h"
#include "Misc/Paths.h"
#include "Shader/ShaderCookedLibrary.h"
#include "Asset/Load.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "FixtureAsset.h"
namespace Durin
{
	class FPluginFixtureConsumerModule final : public IModuleInterface
	{
		auto Record(std::string_view Event) -> void
		{
			if (const char* Path = std::getenv("DURIN_PLUGIN_FIXTURE_TRACE"))
			{
				std::ofstream Stream(Path, std::ios::app);
				Stream << "PluginFixtureConsumer " << Event << "\n";
				if (!Stream) throw std::runtime_error("Cannot write plugin fixture trace");
			}
		}
	public:
		auto StartupModule() -> void override
		{
#if !DURIN_WITH_EDITOR
            if (std::getenv("DURIN_PLUGIN_FIXTURE_COOKED"))
            {
                auto* Component=NewObject<PluginFixture::DFixtureComponent>(nullptr,FName("CookedFixtureComponent"));
                if (!Component || Component->Sample!=42) throw std::runtime_error("Cooked plugin component could not construct");
                std::vector<FShaderRuntimeRequest> Requests;
                const auto Frozen = FreezeShaderRuntimeInventory(EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Requests);
                if (!Frozen) throw std::runtime_error(Frozen.error());
                const auto Found = std::ranges::find(Requests, "PluginFixtureProgram", &FShaderRuntimeRequest::Name);
                if (Found == Requests.end()) throw std::runtime_error("Native plugin Shader is absent from runtime inventory");
                FShaderCookedLibrary Library;
                const auto Opened = FShaderCookedLibrary::Open(std::filesystem::path(FPaths::LaunchDir()) / ShaderCookedLibraryRelativePath,
                    EShaderTargetPlatform::Win64, EShaderTargetProfile::Game, Requests, Library);
                if (!Opened) throw std::runtime_error(Opened.error());
                FShaderCompilerOutput Program;
                if (!Library.Load(*Found, Program) || Program.CompiledShaders.empty()) throw std::runtime_error("Cooked native plugin Shader could not load");
                for (const auto* Name : {"/Game/PluginTypedAsset", "/Plugins/PluginFixtureRuntime/Asset", "/Plugins/PluginFixtureRuntime/Explicit"})
                {
                    std::optional<Durin::UnifiedError::FError> LoadedError;
                    FPackagePath Path;
                    if (!FPackagePath::TryCreate(Name, Path)) throw std::runtime_error("Invalid fixture path");
                    const auto Loaded = LoadPackage(Path, nullptr, &LoadedError);
                    if (!Loaded) throw std::runtime_error((*LoadedError));
                    auto* Asset=Cast<PluginFixture::DFixtureAsset>(Loaded->FindTopLevelAsset(FName(Path.GetPackageName())));
                    if (!Asset || Asset->Value!=42) throw std::runtime_error("Cooked plugin asset behavior differs");
                    if (Path.GetView()=="/Game/PluginTypedAsset")
                    {
                        auto* Consumer=Cast<PluginFixture::DFixtureAsset>(Asset->Reference.Get());
                        auto* Base=Consumer ? Cast<PluginFixture::DFixtureAsset>(Consumer->Reference.Get()) : nullptr;
                        if (!Base || Base->Value!=42 || Base->GetPackage()->GetPackagePath()!="/Plugins/PluginFixtureRuntime/Asset")
                            throw std::runtime_error("Cooked project/plugin reference chain differs");
                    }
                }
                if (!PluginFixture::DFixtureAsset::StaticClass()) throw std::runtime_error("Cooked plugin type is unavailable");
                Record("cooked-assets-shader");
            }
#endif
			Record("start");
			if (const char* Fail = std::getenv("DURIN_PLUGIN_FIXTURE_FAIL");
				Fail && std::string_view(Fail) == "PluginFixtureConsumer")
				throw std::runtime_error("Requested fixture provider failure");
		}
		auto ShutdownModule() -> void override { Record("stop"); }
		auto SupportsDynamicReloading() const -> bool override { return true; }
	};
	IMPLEMENT_MODULE(FPluginFixtureConsumerModule, PluginFixtureConsumer)
}
