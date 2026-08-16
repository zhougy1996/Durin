#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaUIBackend.h"
#include "MonaCoreGlobals.h"
#include "Modules/ModuleManager.h"
#include "RHI.h"

namespace Durin
{
	// Owns the process-wide Mona application while allowing its code to outlive UI shutdown.
	class FMonaModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			Mona::FMonaApplication::Create();
			if (GDynamicRHI && !Mona::InitializeRendering())
			{
				DURIN_ERROR("Mona rendering services failed to initialize.");
			}

			DURIN_DEBUG(STR("Mona platform services initialized successfully."));
		}

		auto ShutdownModule() -> void override
		{
			if (FModuleManager::Get().IsModuleLoaded("MonaImGui"))
			{
				const auto Result = FModuleManager::Get().UnloadModule("MonaImGui");
				if (!Result.Succeeded()) DURIN_ERROR(STR("Failed to unload MonaImGui during Mona shutdown: {}"), Result.Message);
			}

			Mona::FMonaApplication::Shutdown();
			DURIN_DEBUG(STR("Mona shutdown."));
		}
	};

	IMPLEMENT_MODULE(FMonaModule, Mona)
}

namespace Durin::Mona
{
	auto InitializeRendering() -> bool
	{
		if (!FMonaApplication::IsInitialized() || !GDynamicRHI) return false;
		if (FMonaApplication::Get().GetRenderer()) return true;

		FMonaApplication::Get().Initialize();
#if DURIN_WITH_EDITOR
		if (!FModuleManager::Get().LoadModule("MonaImGui"))
		{
			FMonaApplication::Get().ShutdownRenderer();
			return false;
		}
#endif
		DURIN_DEBUG(STR("Mona rendering services initialized successfully."));
		return true;
	}

	auto IsRenderingInitialized() -> bool
	{
		return FMonaApplication::IsInitialized()
			&& FMonaApplication::Get().GetRenderer() != nullptr;
	}

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
