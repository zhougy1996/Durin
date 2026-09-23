#pragma once

#include "MonaAPI.h"
#include "MonaCoreFwd.h"

namespace Durin::Mona
{
	// Called on the game thread after ApplicationCore startup; rendering starts separately.
	MONA_API auto InitializeApplication() -> bool;
	// Safe before initialization. Stop the UI backend before shutting down Mona.
	MONA_API auto Shutdown() -> void;

	// Completes the RHI-dependent half of Mona startup after the platform
	// application and its primary native window are available.
	MONA_API auto InitializeRendering(
		bool bAdoptInitializationPresentationCandidate) -> bool;
	MONA_API auto IsRenderingInitialized() -> bool;

	// Initializes the UI frame. The caller draws application windows before Render().
	MONA_API auto NewFrame() -> void;

	MONA_API auto Render() -> void;

}
