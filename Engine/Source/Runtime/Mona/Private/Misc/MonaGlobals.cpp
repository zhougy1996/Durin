#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaUIBackend.h"
#include "MonaCoreGlobals.h"
#include "Modules/ModuleManager.h"

namespace Durin
{
	// Owns the process-wide Mona application while allowing its code to outlive UI shutdown.
	class FMonaModule final : public IModuleInterface
	{
	public:
		auto StartupModule(FModuleContext&) -> void override
		{
			Mona::FMonaApplication::Create();
			Mona::FMonaApplication::Get().Initialize();

#if DURIN_WITH_EDITOR
			FModuleManager::Get().LoadModule("MonaImGui");
#endif

			DURIN_DEBUG(STR("Mona initialized successfully."));
		}

		auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
#if DURIN_WITH_EDITOR
			const auto Result = FModuleManager::Get().UnloadModule("MonaImGui");
			if (!Result.Succeeded()) DURIN_ERROR(STR("Failed to unload MonaImGui during Mona shutdown: {}"), Result.Message);
#endif

			Mona::FMonaApplication::Shutdown();
			DURIN_DEBUG(STR("Mona shutdown."));
		}
	};

	IMPLEMENT_MODULE(FMonaModule, Mona)
}

namespace Durin::Mona
{
	auto NewFrame() -> void
	{
		if (GActiveUIBackend)
		{
			GActiveUIBackend->NewFrame();
		}
	}

	auto Render() -> void
	{
		if (GActiveUIBackend)
		{
			GActiveUIBackend->Render();
		}
	}

} // namespace Durin::Mona
