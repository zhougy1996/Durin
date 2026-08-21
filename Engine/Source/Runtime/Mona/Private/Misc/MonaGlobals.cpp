#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaUIBackend.h"
#include "MonaCoreGlobals.h"
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
			if (GDynamicRHI && !Mona::InitializeRendering(false))
			{
				DURIN_ERROR("Mona rendering services failed to initialize.");
			}

			DURIN_DEBUG(STR("Mona platform services initialized successfully."));
		}

		auto ShutdownModule() -> void override
		{
			Mona::FMonaApplication::Shutdown();
			DURIN_DEBUG(STR("Mona shutdown."));
		}
	};

	IMPLEMENT_MODULE(FMonaModule, Mona)
}

namespace Durin::Mona
{
	auto InitializeRendering(
		bool bAdoptInitializationPresentationCandidate) -> bool
	{
		if (!FMonaApplication::IsInitialized() || !GDynamicRHI) return false;
		if (FMonaApplication::Get().GetRenderer()) return true;

		FMonaApplication::Get().Initialize(
			bAdoptInitializationPresentationCandidate);
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
		if (IMonaUIBackend* Backend = GetActiveUIBackend())
		{
			Backend->NewFrame();
		}
	}

	auto Render() -> void
	{
		if (IMonaUIBackend* Backend = GetActiveUIBackend())
		{
			Backend->Render();
		}
	}

} // namespace Durin::Mona
