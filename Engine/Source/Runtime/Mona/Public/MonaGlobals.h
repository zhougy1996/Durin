#pragma once

#include "MonaAPI.h"
#include "MonaCoreFwd.h"

namespace Durin::Mona
{
	// Completes the RHI-dependent half of Mona startup after the platform
	// application and its primary native window are available.
	MONA_API auto InitializeRendering() -> bool;
	MONA_API auto IsRenderingInitialized() -> bool;

	MONA_API auto NewFrame() -> void;

	MONA_API auto Render() -> void;

}
