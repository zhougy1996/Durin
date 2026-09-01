#pragma once

#include "DurinEdAPI.h"
#include "Editor/Workspace.h"
#include "Asset/MutationExtensions.h"
#include "Modules/ModularFeature.h"

namespace Durin::Editor
{
	// Couples a workspace implementation with its host descriptor.
	struct FWorkspaceRegistration
	{
		FWorkspaceDescriptor Descriptor;
		std::shared_ptr<IWorkspace> Workspace;
	};

	// Registers a dependency-consistent group of workspaces and asset editors.
	struct FWorkspaceRegistrationBatch
	{
		std::vector<FWorkspaceRegistration> Workspaces;
		std::vector<FAssetEditorRegistration> AssetEditors;
	};

	namespace Detail
	{
		// Shared registry storage kept alive independently from registration handles.
		struct FWorkspaceRegistryState;
	}

	// Owns one registry lease and unregisters its batch when reset or destroyed.
	class FWorkspaceRegistrationHandle
	{
	public:
		FWorkspaceRegistrationHandle() = default;
		DURINED_API ~FWorkspaceRegistrationHandle();
		FWorkspaceRegistrationHandle(const FWorkspaceRegistrationHandle&) = delete;
		auto operator=(const FWorkspaceRegistrationHandle&) -> FWorkspaceRegistrationHandle& = delete;
		DURINED_API FWorkspaceRegistrationHandle(FWorkspaceRegistrationHandle&& Other) noexcept;
		DURINED_API auto operator=(FWorkspaceRegistrationHandle&& Other) noexcept -> FWorkspaceRegistrationHandle&;

		auto IsValid() const -> bool { return RegistrationId != 0 && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
		DURINED_API auto Reset() -> void;

	private:
		friend class FWorkspaceManager;
		DURINED_API FWorkspaceRegistrationHandle(
			std::weak_ptr<Detail::FWorkspaceRegistryState> InState,
			uint64 InRegistrationId
		);

		std::weak_ptr<Detail::FWorkspaceRegistryState> State;
		uint64 RegistrationId = 0;
	};

	// Owns workspace registrations, open documents, and active-host transitions.
	class FWorkspaceManager final : private IAssetMoveObserver
	{
	public:
		DURINED_API FWorkspaceManager();
		DURINED_API ~FWorkspaceManager();
		FWorkspaceManager(const FWorkspaceManager&) = delete;
		auto operator=(const FWorkspaceManager&) -> FWorkspaceManager& = delete;

		DURINED_API auto RegisterBatch(
			FWorkspaceRegistrationBatch Batch,
			FModuleOwnedCallbackGate OwnerGate) -> FWorkspaceRegistrationHandle;
		// Process-owned/test workspaces only; unloadable modules must pass an owner gate.
		auto RegisterBatch(FWorkspaceRegistrationBatch Batch)
			-> FWorkspaceRegistrationHandle
		{
			return RegisterBatch(std::move(Batch), {});
		}
		DURINED_API auto OpenDocument(FDocumentRequest Request) -> FDocumentId;
		DURINED_API auto CompleteDeferredDocumentOpen(FDocumentId DocumentId, bool bSucceeded) -> bool;
		DURINED_API auto OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool;
		DURINED_API auto ActivateDocument(FDocumentId DocumentId) -> bool;
		DURINED_API auto ActivateWorkspace(const FWorkspaceTypeId& WorkspaceType) -> bool;
		DURINED_API auto OpenDefaultWorkspaces() -> bool;
		DURINED_API auto RequestCloseDocument(FDocumentId DocumentId) -> EDocumentCloseResult;
		// Applies one response to the single pending close without losing it on save or discard failure.
		DURINED_API auto ResolvePendingDocumentClose(EDocumentCloseResponse Response) -> EDocumentCloseResult;
		DURINED_API auto RefreshDocumentState() -> void;
		// Updates open and deferred document routing after an authoritative asset move.
		DURINED_API auto RemapResourceId(
			std::string_view SourceResourceId,
			std::string_view DestinationResourceId) -> void;

		DURINED_API auto GetDocuments() const -> const std::vector<FDocumentTab>&;
		DURINED_API auto GetActiveDocument() -> FDocumentTab*;
		DURINED_API auto GetActiveDocument() const -> const FDocumentTab*;
		DURINED_API auto GetPendingCloseDocument() const -> const FDocumentTab*;
		DURINED_API auto FindWorkspace(const FWorkspaceTypeId& WorkspaceType) const -> std::shared_ptr<IWorkspace>;
		DURINED_API auto GetRegisteredWorkspaces() const -> std::vector<std::shared_ptr<IWorkspace>>;
		DURINED_API auto GetWorkspaceDescriptors() const -> std::vector<FWorkspaceDescriptor>;

	private:
		auto OnAssetsRelocated(
			std::span<const FAssetRelocationMapping> Mappings) -> void override;
		auto FindDocument(FDocumentId DocumentId) -> FDocumentTab*;
		auto FindDocument(const FWorkspaceTypeId& WorkspaceType, std::string_view DocumentKey) -> FDocumentTab*;
		auto RequestDeactivateActiveDocument(FDocumentId NextDocumentId = {}) -> bool;
		static auto AssetLabel(std::string_view ResourceId) -> std::string;

		std::shared_ptr<Detail::FWorkspaceRegistryState> State;
		FAssetMoveObserverHandle AssetMoveObserverHandle = 0;
	};
}
