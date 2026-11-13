#pragma once

#include "CoreMinimal.h"
#include "ContentBrowserOperationContext.h"

#include "DObject/AssetPath.h"
#include "DObject/WeakObjectPtr.h"
#include "AssetRegistry/ContentChanges.h"
#include "ContentBrowserAPI.h"
#include "ContentBrowserItem.h"
#include "Import/ImportDialogSupport.h"

namespace Durin
{
	class DAssetEditorSubsystem;
	class DContentBrowserDataSubsystem;
} // namespace Durin
namespace Durin::ContentBrowser
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

	// Owns a selection snapshot. Target directories describe the command destination;
	// PrimaryItem is the clicked item, or the first selected item for details.
	struct FExtensionContext
	{
		std::string VirtualDirectory;
		std::string PhysicalDirectory;
		std::string CurrentVirtualDirectory;
		std::string CurrentPhysicalDirectory;
		std::vector<FContentBrowserItem> Selection;
		std::optional<FContentBrowserItem> PrimaryItem;
		uint64 CatalogRevision = 0;
	};

	// Detail providers return owned text; the host never retains provider UI or asset objects.
	struct FDetailRow
	{
		std::string Label;
		std::string Value;
	};
	// Providers run on the UI thread during preparation, never under a registry lock.
	using FDetailsProvider = std::function<std::vector<FDetailRow>(const FExtensionContext&)>;

	// Independent of the UI surface; unspecified command policies are rejected at registration.
	enum class EContentMutation : uint8 { Unspecified, ReadOnly, MutatesContent };

	// Resolved definition metadata plus independent details/thumbnail contributions.
	struct FAssetTypePresentation
	{
		std::string AssetClassName;
		std::string DisplayName;
		std::string Icon;
		std::string ThumbnailBadgeIcon;
		FDetailsProvider Details;
		std::vector<std::vector<std::string>> CategoryPaths;
		std::array<float, 4> Color = {0.5f, 0.5f, 0.5f, 1.0f};
	};
	CONTENTBROWSER_API auto CaptureAssetCategoryPaths() -> std::vector<std::vector<std::string>>;

	struct FExtensionHost
	{
		std::weak_ptr<void> Lifetime;
		bool bHasLifetime = false;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditors;
		auto IsExpired() const -> bool { return bHasLifetime && Lifetime.expired(); }
		auto Matches(const std::weak_ptr<void>& Other) const -> bool
		{ return !bHasLifetime || (!Lifetime.owner_before(Other) && !Other.owner_before(Lifetime)); }
	};

	// Weak ownership shared by invocation, drawing and host retirement. Never retains a browser.
	class FScopedHostPresentation
	{
	public:
		auto Bind(const FExtensionHost& InHost) -> void { Host = InHost; }
		auto Reset() -> void { Host.reset(); }
		auto IsExpired() const -> bool { return Host && Host->IsExpired(); }
		auto Matches(const FExtensionHost& Other) const -> bool { return !Host || Host->Matches(Other.Lifetime); }
		auto IsOwnedBy(const std::weak_ptr<void>& Other) const -> bool
		{ return Host && Host->bHasLifetime && Host->Matches(Other); }
		template<typename DrawCallback, typename StopCallback>
		auto Draw(const FExtensionHost& Other, DrawCallback DrawUI, StopCallback StopUI) -> void
		{
			if (IsExpired()) { StopUI(); Reset(); return; }
			if (Matches(Other)) DrawUI();
		}
		template<typename StopCallback>
		auto Stop(const std::weak_ptr<void>& Other, StopCallback StopUI) -> void
		{ if (IsOwnedBy(Other)) { StopUI(); Reset(); } }
	private:
		std::optional<FExtensionHost> Host;
	};

	struct FExtensionInvocation
	{
		FExtensionContext Context;
		// False while the host permits inspection but rejects content mutations, including ordinary files.
		bool bAllowAssetMutation = true;
		std::function<bool(std::string_view)> RevealAsset;
		std::function<bool(std::string_view)> RevealDirectory;
		std::function<bool(std::string_view, std::string_view)> OpenAsset;
		std::function<void()> NotifyMountedContentChanged;
		std::function<void(std::string)> ReportError;
		FExtensionHost Host;
	};
	CONTENTBROWSER_API auto MakeImportDialogCallbacks(const FExtensionInvocation& Invocation,
		std::string AssetClassName) -> FImportDialogCallbacks;
	struct FAssetTypeExtras
	{
		std::string AssetClassName;
		std::string ThumbnailBadgeIcon;
		FDetailsProvider Details;
	};

	// Defines one deterministically ordered browser contribution. Owners unregister
	// and release all captured descriptor copies before unloading provider code.
	struct FExtensionDescriptor
	{
		std::string Id;
		std::string Label;
		EExtensionCategory Category = EExtensionCategory::ContextMenu;
		int32 Order = 0;
		EContentMutation Mutation = EContentMutation::Unspecified;
		std::function<bool(const FExtensionContext&)> IsApplicable;
		std::function<void(const FExtensionInvocation&)> Invoke;
		// Required instead of Invoke for Details contributions; details are always read-only.
		FDetailsProvider Details;
		// Draws feature-owned modal state without exposing its concrete module to the host.
		std::function<void(const FExtensionHost&, bool)> DrawHostPresentation;
		// Drains this request owner's dialogs/work before its browser and project retire.
		std::function<void(const std::weak_ptr<void>&)> StopHostPresentation;
		// Assigned by the registry; queued identities cannot bind to a replacement registration.
		uint64 RegistrationSerial = 0;
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

	// Creates an asset only after the browser confirms its final name. The callback
	// must persist exactly this destination and report failure without publishing success.
	struct FAssetCreationDescriptor
	{
		std::string Id;
		std::string Label;
		std::string DefaultName;
		int32 Order = 0;
		std::function<bool(const FTopLevelAssetPath&, std::string&)> Create;
		// If provided, open this asset editor after successful creation and reveal.
		std::string AssetClassNameToOpen;
	};

	CONTENTBROWSER_API auto RegisterAssetCreation(
		FAssetCreationDescriptor Descriptor, std::string& OutError)
		-> FScopedExtensionRegistration;

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

	// A host shares reconciliation across browsers while each browser retains its own cursor.
	struct FMountedContentReconciliationState
	{
		enum class EState : uint8 { Uninitialized, Synchronized, Failed };
		uint64 Revision = 0;
		EState State = EState::Uninitialized;
	};

	// Supplies implementation-neutral services required to construct the browser.
	struct FContentBrowserHostContext
	{
		std::function<bool(const std::string&, const std::string&)> OpenAsset;
		std::function<uint64()> GetMountedContentMutationRevision;
		std::function<void()> NotifyMountedContentMutation;
		std::function<FContentChangeBatch(uint64)> CaptureMountedContentChanges;
		std::function<void(FContentChangeBatch)> NotifyScopedContentMutation;
		std::function<bool()> CanMutateContent;
		// Receives the qualified asset class name. Must not load assets or perform source I/O.
		std::function<FReimportAvailability(std::string_view)> QueryReimport;
		std::function<void(bool, std::string, std::function<void(std::string)>)> Reimport;
		std::shared_ptr<FMountedContentReconciliationState> ReconciliationState;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditors;
		TWeakObjectPtr<DContentBrowserDataSubsystem> DataSubsystem;
		std::function<void(ELogLevel, std::string_view)> ReportDiagnostic;
		operator FContentBrowserOperationContext() const
		{
			return {.OpenAsset = OpenAsset, .NotifyMountedContentMutation = NotifyMountedContentMutation,
				.NotifyScopedContentMutation = NotifyScopedContentMutation, .CanMutateContent = CanMutateContent,
				.AssetEditors = AssetEditors, .ReportDiagnostic = ReportDiagnostic};
		}
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

	CONTENTBROWSER_API auto RegisterAssetTypeExtras(
		FAssetTypeExtras Descriptor, std::string& OutError) -> FScopedExtensionRegistration;
	CONTENTBROWSER_API auto FindAssetTypePresentation(std::string_view QualifiedClassName)
		-> std::optional<FAssetTypePresentation>;
	CONTENTBROWSER_API auto GetPresentationRevision() -> uint64;
	CONTENTBROWSER_API auto StopHostPresentations(const std::weak_ptr<void>& HostLifetime) -> void;
	CONTENTBROWSER_API auto CaptureSelectionDetails(const FExtensionContext& Context)
		-> std::vector<FDetailRow>;
	CONTENTBROWSER_API auto CanInvokeExtension(const FExtensionDescriptor& Descriptor,
		const FExtensionContext& Context, bool bAllowContentMutation) -> bool;

	CONTENTBROWSER_API auto RegisterExtension(
		FExtensionDescriptor Descriptor, std::string& OutError)
		-> FScopedExtensionRegistration;
	CONTENTBROWSER_API auto CaptureExtensions(EExtensionCategory Category)
		-> std::vector<FExtensionDescriptor>;
	// Retain the immutable sorted snapshot while invoking its presenters. Reused until registration changes.
	CONTENTBROWSER_API auto CaptureHostPresenters()
		-> std::shared_ptr<const std::vector<FExtensionDescriptor>>;
	CONTENTBROWSER_API auto InvokeExtension(
		const FExtensionDescriptor& Descriptor,
		const FExtensionInvocation& Invocation) -> bool;
	CONTENTBROWSER_API auto DrawHostPresentation(
		const FExtensionDescriptor& Descriptor,
		bool bAllowAssetMutation, const FExtensionHost& Host = {}) -> bool;
} // namespace Durin::ContentBrowser
