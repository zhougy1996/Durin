#pragma once

#include "AssetForge/Operations/ImportOperation.h"
#include "Editor/Transaction.h"
#include "Modules/ModularFeature.h"
#include "Threading/Task.h"

namespace Durin::Editor::ContentBrowser
{
	// Identifies the extension surface contributed by an asset-family module.
	enum class EExtensionCategory : uint8
	{
		Create,
		Import,
		Reimport,
		Details,
		ContextMenu,
	};

	// Describes whether the browser still accepts externally requested work.
	enum class EAdmissionState : uint8
	{
		Accepting,
		Stopping,
		Stopped,
	};

	struct FExtensionContext
	{
		std::string VirtualDirectory;
		std::string AssetPath;
		std::string AssetClassName;
	};

	struct FExtensionInvocation
	{
		FExtensionContext Context;
	};

	// Defines one deterministically ordered, unload-gated browser contribution.
	struct FExtensionDescriptor
	{
		std::string Id;
		EExtensionCategory Category = EExtensionCategory::ContextMenu;
		int32 Order = 0;
		std::function<bool(const FExtensionContext&)> IsApplicable;
		std::function<void(const FExtensionInvocation&)> Invoke;
		FModuleOwnedCallbackGate OwnerGate;
	};

	// Removes one contribution when its owning feature module releases the handle.
	class FScopedExtensionRegistration
	{
	public:
		FScopedExtensionRegistration() = default;
		explicit FScopedExtensionRegistration(std::function<void()> InRelease)
			: Release(std::move(InRelease))
		{
		}
		FScopedExtensionRegistration(const FScopedExtensionRegistration&) = delete;
		auto operator=(const FScopedExtensionRegistration&)
			-> FScopedExtensionRegistration& = delete;
		FScopedExtensionRegistration(FScopedExtensionRegistration&& Other) noexcept
			: Release(std::exchange(Other.Release, {}))
		{
		}
		auto operator=(FScopedExtensionRegistration&& Other) noexcept
			-> FScopedExtensionRegistration&
		{
			if (this == &Other) return *this;
			Reset();
			Release = std::exchange(Other.Release, {});
			return *this;
		}
		~FScopedExtensionRegistration() { Reset(); }

		auto IsValid() const -> bool { return static_cast<bool>(Release); }
		auto Reset() -> void
		{
			if (std::function<void()> Callback = std::exchange(Release, {})) Callback();
		}

	private:
		std::function<void()> Release;
	};

	struct FAssetMove
	{
		std::string OldPath;
		std::string NewPath;
	};

	struct FActionResult
	{
		bool bSucceeded = false;
		std::string Message;
	};

	// Supplies implementation-neutral services required to construct the browser.
	struct FConstructionServices
	{
		std::function<bool(const std::string&, const std::string&)> OpenAsset;
		std::function<bool(std::unique_ptr<ITransaction>)> ExecuteTransaction;
		std::function<uint64()> GetMountedContentMutationRevision;
		std::function<void()> NotifyMountedContentMutation;
		std::function<FActionResult(std::span<const FAssetMove>)> MoveAssets;
		std::function<void(AssetForge::FImportOperationHandle, std::string)>
			NotifyImportStarted;
		FTaskScopeToken ThumbnailTaskScope;
		FModuleOwnedCallbackGate OwnerGate;
	};

	// Exposes host-level browser requests without leaking models or UI types.
	class IContentBrowser
	{
	public:
		virtual ~IContentBrowser() = default;
		virtual auto RevealAsset(std::string_view AssetPath) -> bool = 0;
		virtual auto RevealDirectory(std::string_view DirectoryPath) -> bool = 0;
		virtual auto RequestFocus() -> bool = 0;
		virtual auto NotifyMountedContentChanged() -> bool = 0;
		virtual auto StopRequestAdmission() -> void = 0;
		virtual auto GetAdmissionState() const -> EAdmissionState = 0;
	};
} // namespace Durin::Editor::ContentBrowser
