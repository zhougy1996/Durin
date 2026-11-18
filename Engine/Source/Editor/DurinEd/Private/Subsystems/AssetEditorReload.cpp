#include "Subsystems/AssetEditorSubsystem.h"

#include "Asset/Load.h"
#include "Asset/PackageReload.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/ToolkitManager.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	namespace
	{
		// Keeps the admitted Toolkit and its current association stable across preparation.
		class FEditorReloadParticipant final : public IPackageReloadParticipant
		{
		public:
			explicit FEditorReloadParticipant(std::shared_ptr<FAssetEditorToolkit> InToolkit)
				: Toolkit(std::move(InToolkit))
				, Participant(Toolkit->CreateReloadParticipant())
			{
				for (auto* Object : Toolkit->GetEditingObjects()) Objects.emplace_back(Object);
			}

			auto MatchesEditor() const -> bool
			{
				if (!Toolkit->IsEditorOpen() || Toolkit->GetEditorInstance() != Instance) return false;
				const auto Current = Toolkit->GetEditingObjects();
				return std::ranges::is_permutation(Current, Objects, {}, [](auto* Object) { return FObjectKey(Object); });
			}

			auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
			{
				if (!MatchesEditor() || !Toolkit->RequestDeactivate() || !MatchesEditor())
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Busy});
				if (!Participant)
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Unsupported});
				return Participant->Prepare(Map);
			}

			auto Validate() const -> bool override { return MatchesEditor() && Participant->Validate(); }
			auto CoversNativeReferences(const DObject& Owner) const -> bool override
			{ return Participant->CoversNativeReferences(Owner); }
			auto GetStrongReferenceCount(const DObject& Object) const -> uint32 override
			{ return Participant->GetStrongReferenceCount(Object); }
			auto Commit() noexcept -> void override { Participant->Commit(); }
			auto Abort() noexcept -> void override { if (Participant) Participant->Abort(); }
			auto CanRetire() const -> bool override { return Participant->CanRetire(); }

		private:
			std::shared_ptr<FAssetEditorToolkit> Toolkit;
			std::shared_ptr<IObjectReplacementParticipant> Participant;
			IAssetEditorInstance* Instance = Toolkit->GetEditorInstance();
			std::vector<FObjectKey> Objects;
		};
	}

	auto DAssetEditorSubsystem::AddPackageReloadParticipants(FPackageReloadRequest& Request) -> bool
	{
		if (!CanCall()) return false;
		auto* Transactions = GetEditor()->GetTransactor();
		auto History = Transactions ? CreateTransactorReloadParticipant(*Transactions, Request.Packages) : nullptr;
		if (Transactions && !History) return false;

		std::unordered_set<IAssetEditorInstance*> AffectedEditors;
		for (const auto& [Key, Editors] : OpenedAssets)
			if (const auto* Object = Key.ResolveObjectPtr(); Object && std::ranges::contains(Request.Packages, Object->GetPackage()))
				AffectedEditors.insert(Editors.begin(), Editors.end());
		std::vector<std::shared_ptr<FAssetEditorToolkit>> Affected;
		for (const auto& Toolkit : FToolkitManager::Get().GetToolkits(this))
			if (AffectedEditors.contains(Toolkit->GetEditorInstance()))
			{
				Request.Participants.push_back(std::make_shared<FEditorReloadParticipant>(Toolkit));
				Affected.push_back(Toolkit);
			}
		if (History) Request.Participants.push_back(std::move(History));
		if (Affected.empty()) return true;

		std::vector<std::pair<DPackage*, FPackagePath>> Packages;
		for (auto* Package : Request.Packages)
			if (IsValid(Package) && std::ranges::none_of(Packages, [Package](const auto& Entry) { return Entry.first == Package; }))
				Packages.emplace_back(Package, Package->GetPackagePathIdentity());
		Request.OnPublished = [Weak = TWeakObjectPtr(this), Affected = std::move(Affected), Packages = std::move(Packages),
			PreviousCallback = std::move(Request.OnPublished)] {
			auto* Owner = Weak.Get();
			if (!Owner || !Owner->CanCall()) return;
			std::vector<FAssetEditorEvent> Events;
			for (const auto& Toolkit : Affected)
			{
				if (!Owner->OwnsToolkit(*Toolkit) || Toolkit->IsClosed()) continue;
				for (const auto& [Previous, Path] : Packages)
					if (auto* Replacement = FindResidentPackage(Path); Replacement && Replacement != Previous)
						Toolkit->OnPackageReloaded(Previous, Replacement);
			}
			for (const auto& Toolkit : Affected) Owner->SynchronizeToolkitAssets(*Toolkit, &Events);
			for (auto& Event : Events)
			{
				if (Event.Type == EAssetEditorEvent::Opened && std::ranges::none_of(Affected, [&](const auto& Toolkit) {
					return Toolkit->IsEditorOpen() && Toolkit->GetWindowId() == Event.InstanceId;
				})) continue;
				Owner->PublishEvent(std::move(Event));
			}
			if (PreviousCallback) PreviousCallback();
		};
		return true;
	}
}
