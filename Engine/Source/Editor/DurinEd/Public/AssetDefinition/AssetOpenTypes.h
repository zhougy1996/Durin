#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/AssetPath.h"

namespace Durin
{
	class DObject;
	class DAssetEditorSubsystem;
	class IToolkitHost;

	// Separates inspection from editing before a session or asset is loaded.
	enum class EAssetOpenMethod : uint8 { Edit, View };
	enum class EToolkitMode : uint8 { Standalone, WorldCentric };

	struct FAssetOpenSupportArgs
	{
		EAssetOpenMethod OpenMethod = EAssetOpenMethod::Edit;
	};

	// Metadata-only support, including context required before loading or constructing UI.
	struct FAssetOpenSupport
	{
		EAssetOpenMethod OpenMethod = EAssetOpenMethod::Edit;
		bool IsSupported = false;
		std::optional<EToolkitMode> RequiredToolkitMode;

		auto ResolveToolkitMode(EToolkitMode Requested, std::optional<EToolkitMode> HostMode = {}) const -> std::optional<EToolkitMode>
		{
			const auto ValidMode = [](EToolkitMode Mode) { return Mode == EToolkitMode::Standalone || Mode == EToolkitMode::WorldCentric; };
			if (!IsSupported || (OpenMethod != EAssetOpenMethod::Edit && OpenMethod != EAssetOpenMethod::View)
				|| !ValidMode(Requested) || (HostMode && !ValidMode(*HostMode))
				|| (RequiredToolkitMode && !ValidMode(*RequiredToolkitMode))) return {};
			const auto Effective = RequiredToolkitMode.value_or(HostMode.value_or(Requested));
			return HostMode && *HostMode != Effective ? std::nullopt : std::optional(Effective);
		}
	};

	inline auto GetDefaultAssetOpenSupport(const FAssetOpenSupportArgs& Args) -> FAssetOpenSupport
	{ return {Args.OpenMethod, Args.OpenMethod == EAssetOpenMethod::Edit || Args.OpenMethod == EAssetOpenMethod::View}; }

	// Metadata selection; querying support must not load the selected package.
	struct FAssetOpenTarget
	{
		FSoftObjectPath AssetPath;
		std::string AssetClassName;
	};

	// Pending is accepted work, not a committed editor association.
	enum class EAssetOpenStatus : uint8
	{
		Unhandled, Rejected, Pending, Completed, Failed, Cancelled,
	};

	struct FAssetOpenRequestId
	{
		uint64 Value = 0;
		auto IsValid() const -> bool { return Value != 0; }
		auto operator==(const FAssetOpenRequestId&) const -> bool = default;
	};

	// Owns one input's outcome independently from the rest of a batch.
	struct FAssetOpenItemResult
	{
		FAssetOpenTarget Target;
		EAssetOpenStatus Status = EAssetOpenStatus::Unhandled;
		FAssetOpenRequestId RequestId;
		std::string Message;
		auto IsAccepted() const -> bool
		{ return Status == EAssetOpenStatus::Pending || Status == EAssetOpenStatus::Completed; }
	};

	struct FAssetOpenResult
	{
		std::vector<FAssetOpenItemResult> Items;
		auto IsAccepted() const -> bool
		{
			return !Items.empty() && std::ranges::all_of(Items,
				[](const auto& Item) { return Item.IsAccepted(); });
		}
	};

	// Explicit editor context and execution-only loading keep definitions stateless.
	struct FAssetOpenArgs
	{
		DAssetEditorSubsystem* AssetEditors = nullptr;
		std::vector<FAssetOpenTarget> Assets;
		EAssetOpenMethod OpenMethod = EAssetOpenMethod::Edit;
		EToolkitMode ToolkitMode = EToolkitMode::Standalone;
		std::shared_ptr<IToolkitHost> Host;
		std::string Origin;
		std::string SessionRole;
		bool bPrimary = true;
		// Execute this selection as one compatible session, e.g. grouped restore.
		bool bCreateSingleSession = false;
		bool bIsRestore = false;

		// Caller retains the loaded object before invoking any callbacks or GC.
		DURINED_API auto LoadAsset(const FAssetOpenTarget& Target) const -> DObject*;
	};
}
