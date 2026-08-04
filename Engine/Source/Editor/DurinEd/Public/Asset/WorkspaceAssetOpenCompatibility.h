#pragma once

#include "AssetSystem.h"
#include "DurinEdAPI.h"

namespace Durin
{
	// Applies the shared editor policy to one asset load before document activation.
	class FWorkspaceAssetOpenCompatibility
	{
	public:
		using FReleaseIntroducedPackages = std::function<Asset::FAssetResult()>;

		DURINED_API explicit FWorkspaceAssetOpenCompatibility(const FAssetPath& RequestedPath);
		DURINED_API FWorkspaceAssetOpenCompatibility(
			FAssetPath RequestedPath,
			FReleaseIntroducedPackages ReleaseIntroducedPackages);

		DURINED_API auto RejectIfIncompatible(
			const Asset::FAssetLoadReport& Report,
			std::string& OutDiagnostic) -> bool;
		DURINED_API auto ReleaseIntroducedPackages() -> Asset::FAssetResult;

	private:
		FAssetPath RequestedPath;
		FReleaseIntroducedPackages Release;
	};
}
