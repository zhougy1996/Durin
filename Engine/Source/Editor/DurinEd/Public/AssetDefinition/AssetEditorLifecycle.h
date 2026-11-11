#pragma once

#include "CoreMinimal.h"
#include "AssetDefinition/AssetOpenTypes.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	// Value notifications survive supported business reentry, such as closing other
	// sessions or withdrawing subscriptions. Callbacks request GC/shutdown; owners
	// collect and tear down after the complete call chain returns.
	enum class EAssetEditorEvent : uint8 { Requested, RequestFinished, Opened, Focused, CloseRequested, AssetRemoved, Closed };
	struct FAssetEditorEvent
	{
		EAssetEditorEvent Type = EAssetEditorEvent::Requested;
		FAssetOpenTarget Target;
		FObjectKey Session;
		EAssetOpenMethod Method = EAssetOpenMethod::Edit;
		EAssetOpenStatus Status = EAssetOpenStatus::Unhandled;
		FAssetOpenRequestId RequestId;
	};
	struct FAssetEditorUsage
	{
		uint64 Opened = 0;
		uint64 Focused = 0;
		uint64 Closed = 0;
		double EditingSeconds = 0.0;
	};
	// Non-owning cancellation token; destruction withdraws a subscription.
	class FAssetEditorSubscription
	{
	public:
		FAssetEditorSubscription() = default;
		explicit FAssetEditorSubscription(std::function<void()> InRelease) : Release(std::move(InRelease)) {}
		~FAssetEditorSubscription() { Reset(); }
		FAssetEditorSubscription(const FAssetEditorSubscription&) = delete;
		auto operator=(const FAssetEditorSubscription&) -> FAssetEditorSubscription& = delete;
		FAssetEditorSubscription(FAssetEditorSubscription&& Other) noexcept : Release(std::exchange(Other.Release, {})) {}
		auto operator=(FAssetEditorSubscription&& Other) noexcept -> FAssetEditorSubscription&
		{ if (this != &Other) { Reset(); Release = std::exchange(Other.Release, {}); } return *this; }
		auto Reset() -> void { if (auto Callback = std::exchange(Release, {})) Callback(); }
	private:
		std::function<void()> Release;
	};
	// Serialized identities contain no object pointers, live IDs or provider leases.
	struct FAssetEditorRestoreEntry
	{
		std::vector<FAssetOpenTarget> Assets;
		EAssetOpenMethod Method = EAssetOpenMethod::Edit;
		EToolkitMode Mode = EToolkitMode::Standalone;
		std::string Role;
		bool bPrimary = true;
	};
}
