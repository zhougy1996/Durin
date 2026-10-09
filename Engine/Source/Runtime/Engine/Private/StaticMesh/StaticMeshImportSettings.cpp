#include "StaticMesh/StaticMeshImportSettings.h"

#include "Math/Vector.h"

namespace Durin
{
	namespace
	{
		auto ImportAxisVector(EStaticMeshImportAxis Axis, FVector3f& OutVector, uint32& OutComponent) -> bool
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: OutVector = FVector3f(1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::NegativeX: OutVector = FVector3f(-1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::PositiveY: OutVector = FVector3f(0.0f, 1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::NegativeY: OutVector = FVector3f(0.0f, -1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::PositiveZ: OutVector = FVector3f(0.0f, 0.0f, 1.0f); OutComponent = 2; return true;
			case EStaticMeshImportAxis::NegativeZ: OutVector = FVector3f(0.0f, 0.0f, -1.0f); OutComponent = 2; return true;
			}
			return false;
		}
	}

	auto FormatStaticMeshImportSettingsError(const FStaticMeshImportSettingsError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshImportSettingsError::None: return {};
		case EStaticMeshImportSettingsError::UnknownAxis: return "The import coordinate system contains an unknown axis.";
		case EStaticMeshImportSettingsError::RepeatedAxis: return "Forward, Right, and Up must use X, Y, and Z exactly once.";
		}
		return {};
	}

	auto FStaticMeshImportSettings::Validate() const -> std::expected<void, FStaticMeshImportSettingsError>
	{
		FVector3f UnusedVector;
		uint32 ForwardComponent = 0;
		uint32 RightComponent = 0;
		uint32 UpComponent = 0;
		const bool bAxesKnown = ImportAxisVector(ForwardAxis, UnusedVector, ForwardComponent)
			&& ImportAxisVector(RightAxis, UnusedVector, RightComponent)
			&& ImportAxisVector(UpAxis, UnusedVector, UpComponent);
		if (!bAxesKnown)
			return std::unexpected(FStaticMeshImportSettingsError{.Code = EStaticMeshImportSettingsError::UnknownAxis,
				.ForwardAxis = ForwardAxis, .RightAxis = RightAxis, .UpAxis = UpAxis});
		if (ForwardComponent == RightComponent || ForwardComponent == UpComponent || RightComponent == UpComponent)
			return std::unexpected(FStaticMeshImportSettingsError{.Code = EStaticMeshImportSettingsError::RepeatedAxis,
				.ForwardAxis = ForwardAxis, .RightAxis = RightAxis, .UpAxis = UpAxis});
		return {};
	}

	auto FStaticMeshImportSettings::MakeDurin() -> FStaticMeshImportSettings
	{
		return {};
	}

	auto FStaticMeshImportSettings::MakeYUpNegativeZForward() -> FStaticMeshImportSettings
	{
		return {
			.ForwardAxis = EStaticMeshImportAxis::NegativeZ,
			.RightAxis = EStaticMeshImportAxis::PositiveX,
			.UpAxis = EStaticMeshImportAxis::PositiveY
		};
	}
}
