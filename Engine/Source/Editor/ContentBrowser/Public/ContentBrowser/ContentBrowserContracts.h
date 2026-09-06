#pragma once

#include "DObject/AssetPath.h"
#include "Threading/Task.h"
#include "ContentBrowserAPI.h"

namespace Durin::Editor::ContentBrowser
{
	// Identifies the extension surface contributed by an asset-family module.
	enum class EExtensionCategory : uint8
	{
		Create,
		Details,
		ContextMenu,
		Import,
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
		// False while the host permits inspection but rejects asset mutations.
		bool bAllowAssetMutation = true;
		std::function<bool(std::string_view)> RevealAsset;
		std::function<bool(std::string_view)> RevealDirectory;
		std::function<bool(std::string_view, std::string_view)> OpenAsset;
		std::function<void()> NotifyMountedContentChanged;
		std::function<void(std::string)> ReportError;
	};

	// Defines one deterministically ordered browser contribution. Owners unregister
	// and release all captured descriptor copies before unloading provider code.
	struct FExtensionDescriptor
	{
		std::string Id;
		std::string Label;
		EExtensionCategory Category = EExtensionCategory::ContextMenu;
		int32 Order = 0;
		std::function<bool(const FExtensionContext&)> IsApplicable;
		std::function<void(const FExtensionInvocation&)> Invoke;
		// Draws feature-owned modal state without exposing its concrete module to the host.
		std::function<void(bool)> DrawHostPresentation;
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
		FPackagePath OldPath;
		FPackagePath NewPath;
	};

	struct FActionResult
	{
		bool bSucceeded = false;
		std::string Message;
	};

	// Candidate actions from class metadata; source availability is checked on execution.
	struct FReimportAvailability
	{
		bool bCanReimport = false;
		bool bCanReimportFromFile = false;
	};

	// Supplies implementation-neutral services required to construct the browser.
	struct FConstructionServices
	{
		std::function<bool(const std::string&, const std::string&)> OpenAsset;
		std::function<uint64()> GetMountedContentMutationRevision;
		std::function<void()> NotifyMountedContentMutation;
		std::function<FActionResult(std::span<const FAssetMove>)> MoveAssets;
		std::function<FActionResult(std::span<const FPackagePath>)> FixUpRedirectors;
		// Receives the qualified asset class name. Must not load assets or perform source I/O.
		std::function<FReimportAvailability(std::string_view)> QueryReimport;
		std::function<void(bool, std::string, std::function<void(std::string)>)> Reimport;
		FTaskScopeToken ThumbnailTaskScope;
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

	CONTENTBROWSER_API auto RegisterExtension(
		FExtensionDescriptor Descriptor, std::string& OutError)
		-> FScopedExtensionRegistration;
	CONTENTBROWSER_API auto CaptureExtensions(EExtensionCategory Category)
		-> std::vector<FExtensionDescriptor>;
	CONTENTBROWSER_API auto CaptureHostPresenters()
		-> std::vector<FExtensionDescriptor>;
	CONTENTBROWSER_API auto InvokeExtension(
		const FExtensionDescriptor& Descriptor,
		const FExtensionInvocation& Invocation) -> bool;
	CONTENTBROWSER_API auto DrawHostPresentation(
		const FExtensionDescriptor& Descriptor,
		bool bAllowAssetMutation) -> bool;
} // namespace Durin::Editor::ContentBrowser
