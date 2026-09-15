#pragma once

#include "Serialization/CustomVersion.h"
#include "EngineAPI.h"

namespace Durin
{
	class FArchive;

	// Versions authored source metadata; standalone geometry bulk has its own codec version.
	struct FStaticMeshSourceVersion
	{
		static constexpr FGuid Guid{0xc4d875e2, 0x610947ac, 0x9b250ead, 0xf15638b4};
		static constexpr int32 CurrentVersion = 1;
		ENGINE_API static auto Serialize(FArchive& Ar) -> bool;
	};
}
