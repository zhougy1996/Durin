#pragma once

#include "DurinEdAPI.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor
{
	class FMeshCoordinateImportModel
	{
	public:
		enum class EPreset : uint8
		{
			Durin,
			YUpNegativeZForward,
			Custom
		};

		DURINED_API auto Reset() -> void;
		DURINED_API auto SetPreset(EPreset InPreset) -> void;
		DURINED_API auto Draw() -> void;
		auto GetSettings() -> FStaticMeshImportSettings& { return Settings; }
		auto GetSettings() const -> const FStaticMeshImportSettings& { return Settings; }

	private:
		FStaticMeshImportSettings Settings = FStaticMeshImportSettings::MakeDurin();
		EPreset Preset = EPreset::Durin;
	};
}
