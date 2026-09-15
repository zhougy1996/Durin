#pragma once

#include "EngineAPI.h"
#include "Misc/Guid.h"

namespace Durin
{
	class FArchive;

	// Shared package baseline for material and function expression graphs and their cooked projection.
	struct FMaterialGraphVersion
	{
		static constexpr FGuid Guid{0x60f2b514, 0xb4f546a1, 0x890d2268, 0xceb304f7};
		static constexpr int32 CurrentVersion = 1;
		// Declares package writes and checks package reads; in-memory copies do not need a file version.
		ENGINE_API static auto Serialize(FArchive& Ar) -> bool;
	};

	// Independent package baseline for typed material-instance parameter overrides.
	struct FMaterialInstanceVersion
	{
		static constexpr FGuid Guid{0x9e8247bc, 0x4f0f48d6, 0xb3167d09, 0x56ade221};
		static constexpr int32 CurrentVersion = 1;
		// Uses the same package-only policy as the material graph domain.
		ENGINE_API static auto Serialize(FArchive& Ar) -> bool;
	};
}
