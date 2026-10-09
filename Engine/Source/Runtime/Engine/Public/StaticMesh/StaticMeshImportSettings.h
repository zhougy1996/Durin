#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"

#include "StaticMeshImportSettings.gen.h"

namespace Durin
{
	// Selects a signed source axis when converting imported geometry to Durin space.
	DENUM()
	enum class EStaticMeshImportAxis : int8
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ
	};

	enum class EStaticMeshImportSettingsError : uint8 { None, UnknownAxis, RepeatedAxis };
	struct FStaticMeshImportSettingsError
	{
		EStaticMeshImportSettingsError Code = EStaticMeshImportSettingsError::None;
		EStaticMeshImportAxis ForwardAxis = EStaticMeshImportAxis::PositiveX;
		EStaticMeshImportAxis RightAxis = EStaticMeshImportAxis::PositiveY;
		EStaticMeshImportAxis UpAxis = EStaticMeshImportAxis::PositiveZ;
	};

	ENGINE_API auto FormatStaticMeshImportSettingsError(const FStaticMeshImportSettingsError& Error) -> std::string;

	// Defines the orthogonal source basis used during static-mesh import.
	DSTRUCT()
	struct FStaticMeshImportSettings
	{
		GENERATED_BODY()

		DPROPERTY()
		EStaticMeshImportAxis ForwardAxis = EStaticMeshImportAxis::PositiveX;

		DPROPERTY()
		EStaticMeshImportAxis RightAxis = EStaticMeshImportAxis::PositiveY;

		DPROPERTY()
		EStaticMeshImportAxis UpAxis = EStaticMeshImportAxis::PositiveZ;

		ENGINE_API auto Validate() const -> std::expected<void, FStaticMeshImportSettingsError>;

		ENGINE_API static auto MakeDurin() -> FStaticMeshImportSettings;
		ENGINE_API static auto MakeYUpNegativeZForward() -> FStaticMeshImportSettings;

		auto operator==(const FStaticMeshImportSettings&) const -> bool = default;
	};
}
