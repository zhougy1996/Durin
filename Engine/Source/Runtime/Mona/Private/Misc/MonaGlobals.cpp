#include "MonaGlobals.h"

#include "Application/MonaApplication.h"
#include "MonaUIBackend.h"
#include "MonaCoreGlobals.h"
#include "RHI.h"
#include "ApplicationCore.h"

namespace Durin::Mona
{
	auto InitializeApplication() -> bool
	{
		if (FMonaApplication::IsInitialized()) return true;
		if (!IsApplicationCoreInitialized()) return false;
		FMonaApplication::Create();
		DURIN_DEBUG(STR("Mona platform services initialized successfully."));
		return true;
	}

	auto Shutdown() -> void
	{
		if (!FMonaApplication::IsInitialized()) return;
		FMonaApplication::Shutdown();
		DURIN_DEBUG(STR("Mona shutdown."));
	}

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
