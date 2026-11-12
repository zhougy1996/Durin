#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "AssetToolsAPI.h"
#include "PackageTools.gen.h"

namespace Durin
{
	class DPackage;
	DCLASS(Abstract)
	class DPackageTools : public DObject
	{
		GENERATED_BODY()
	public:
		struct FUnloadPackageParams
		{
			explicit FUnloadPackageParams(std::span<DPackage* const> InPackages)
				: Packages(InPackages.begin(), InPackages.end()) {}
			std::vector<DPackage*> Packages;
			bool bUnloadDirtyPackages = false;
			bool bResetTransBuffer = true;
			std::string OutErrorMessage;
		};
		// True only when at least one candidate package left the resident set.
		// Empty/invalid requests return false; partial success retains diagnostics.
		ASSETTOOLS_API static auto UnloadPackages(std::span<DPackage* const> Packages) -> bool;
		ASSETTOOLS_API static auto UnloadPackages(std::span<DPackage* const> Packages,
			std::string& OutErrorMessage, bool bUnloadDirtyPackages = false) -> bool;
		ASSETTOOLS_API static auto UnloadPackages(FUnloadPackageParams& Params) -> bool;
	};
}
