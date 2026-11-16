#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "AssetToolsAPI.h"
#include "PackageTools.gen.h"

namespace Durin
{
	class DPackage;
	// Interactive hosts decide whether authored changes may be abandoned by reload.
	enum class EReloadPackagesInteractionMode : uint8 { Interactive, AssumePositive, AssumeNegative };
	DCLASS(Abstract)
	class DPackageTools : public DObject
	{
		GENERATED_BODY()
	public:
		struct FUnloadPackageParams
		{
			explicit FUnloadPackageParams(std::span<DPackage* const> InPackages)
				: Packages(InPackages.begin(), InPackages.end()) {}
			const std::vector<DPackage*> Packages;
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
		using FReloadPrompt = std::function<bool(std::span<DPackage* const>)>;
		ASSETTOOLS_API static auto SetReloadPrompt(FReloadPrompt Prompt) -> void;
		ASSETTOOLS_API static auto ReloadPackages(std::span<DPackage* const> Packages) -> bool;
		ASSETTOOLS_API static auto ReloadPackages(std::span<DPackage* const> Packages,
			std::string& OutErrorMessage, EReloadPackagesInteractionMode Mode = EReloadPackagesInteractionMode::Interactive) -> bool;
	};
}
