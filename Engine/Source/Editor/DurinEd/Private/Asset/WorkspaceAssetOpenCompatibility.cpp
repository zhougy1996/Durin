#include "Asset/WorkspaceAssetOpenCompatibility.h"

namespace Durin::Editor
{
	FWorkspaceAssetOpenCompatibility::FWorkspaceAssetOpenCompatibility(
		const FAssetPath& InRequestedPath)
		: FWorkspaceAssetOpenCompatibility(
			InRequestedPath,
			[Snapshot = Asset::CapturePackageLoadSnapshot()] {
				return Asset::ReleasePackagesLoadedSince(Snapshot);
			})
	{
	}

	FWorkspaceAssetOpenCompatibility::FWorkspaceAssetOpenCompatibility(
		FAssetPath InRequestedPath,
		FReleaseIntroducedPackages InReleaseIntroducedPackages)
		: RequestedPath(std::move(InRequestedPath))
		, Release(std::move(InReleaseIntroducedPackages))
	{
	}

	auto FWorkspaceAssetOpenCompatibility::RejectIfIncompatible(
		const Asset::FAssetLoadReport& Report,
		std::string& OutDiagnostic) -> bool
	{
		if (!Report.HasCompatibilityIssues()) return false;

		const FAssetPath& PackagePath = Report.PackagePath.IsValid()
			? Report.PackagePath
			: RequestedPath;
		OutDiagnostic = std::format(
			"Asset {} is incompatible with the current authored baseline and was not opened. "
			"Run Asset Compatibility Audit for complete details.",
			PackagePath.ToString());
		const Asset::FAssetResult ReleaseResult = ReleaseIntroducedPackages();
		if (!ReleaseResult)
			DURIN_WARN(
				"Failed to release packages introduced by rejected asset open '{}': {}",
				PackagePath.ToString(),
				ReleaseResult.Message);
		return true;
	}

	auto FWorkspaceAssetOpenCompatibility::ReleaseIntroducedPackages()
		-> Asset::FAssetResult
	{
		return Release ? Release() : Asset::FAssetResult{};
	}
} // namespace Durin::Editor
