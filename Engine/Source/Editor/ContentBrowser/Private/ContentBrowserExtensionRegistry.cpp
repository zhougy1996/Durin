#include "ContentBrowser/ContentBrowserContracts.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transactor.h"

namespace Durin::ContentBrowser
{
	namespace
	{
		auto DispatchHostCallback(const TWeakObjectPtr<DAssetEditorSubsystem>& Owner, std::function<void()> Callback) -> bool
		{
			if (Owner.GetKey() == FObjectKey{}) { Callback(); return true; }
			auto* Service = Owner.Get();
			if (!Service || !IsValid(Service->GetEditor()) || Service->GetEditor()->IsShutdownRequested()
				|| Service->GetEditor()->GetEditorSubsystem<DAssetEditorSubsystem>() != Service) return false;
			bool Invoked = false;
			auto* Editor = Service->GetEditor();
			Editor->DispatchSubsystemCallback([&] {
				FScopedEditorTransactionContext Context(Editor);
				Invoked = true;
				Callback();
			});
			return Invoked;
		}
	}
	auto MakeImportDialogCallbacks(const FExtensionInvocation& Invocation, std::string ClassName) -> FImportDialogCallbacks
	{
		auto IsAlive = [Lifetime = Invocation.HostLifetime, Bound = Invocation.bHasHostLifetime] { return !Bound || !Lifetime.expired(); };
		return {
			.ClearError = [IsAlive, Report = Invocation.ReportError] { if (IsAlive() && Report) Report({}); },
			.ReportError = [IsAlive, Report = Invocation.ReportError](std::string Message) { if (IsAlive() && Report) Report(std::move(Message)); },
			.AssetCreated = [IsAlive, Reveal = Invocation.RevealAsset, Open = Invocation.OpenAsset, ClassName](std::string Path) {
				if (!IsAlive()) return;
				if (Reveal) Reveal(Path); if (Open && !ClassName.empty()) Open(Path, ClassName);
			},
			.ImportedDirectory = [IsAlive, Reveal = Invocation.RevealDirectory](std::string Path) {
				if (!IsAlive()) return;
				if (Reveal) Reveal(Path);
			}};
	}
	namespace
	{
		struct FRegistryState
		{
			std::mutex Mutex;
			std::unordered_map<std::string, FExtensionDescriptor> Entries;
			std::shared_ptr<const std::vector<FExtensionDescriptor>> HostPresenters;
			std::unordered_map<std::string, FAssetTypeExtras> Extras;
			uint64 PresentationRevision = 0;
			uint64 NextExtensionSerial = 1;
		};

		auto GetRegistryState() -> std::shared_ptr<FRegistryState>
		{
			static const std::shared_ptr<FRegistryState> State =
				std::make_shared<FRegistryState>();
			return State;
		}
	}

	auto RegisterExtension(
		FExtensionDescriptor Descriptor, std::string& OutError)
		-> FScopedExtensionRegistration
	{
		OutError.clear();
		const bool bDetails = Descriptor.Category == EExtensionCategory::Details;
		if (Descriptor.Id.empty() || Descriptor.Label.empty() || !Descriptor.IsApplicable
			|| (bDetails ? (!Descriptor.Details || Descriptor.Invoke
				|| Descriptor.Mutation != EContentMutation::ReadOnly)
				: (!Descriptor.Invoke || Descriptor.Details
					|| Descriptor.Mutation == EContentMutation::Unspecified)))
		{
			OutError = "Extensions require an ID, label, applicability and explicit mutation policy; details require a read-only provider instead of a command.";
			return {};
		}
		const std::shared_ptr<FRegistryState> State = GetRegistryState();
		const std::string Id = Descriptor.Id;
		{
			std::scoped_lock Lock(State->Mutex);
			if (State->Entries.contains(Descriptor.Id))
			{
				OutError = std::format(
					"Content Browser extension '{}' is already registered.",
					Descriptor.Id);
				return {};
			}
			Descriptor.RegistrationSerial = State->NextExtensionSerial++;
			if (Descriptor.DrawHostPresentation || Descriptor.StopHostPresentation) State->HostPresenters.reset();
			State->Entries.emplace(Descriptor.Id, std::move(Descriptor));
		}
		return FScopedExtensionRegistration([WeakState = std::weak_ptr(State), Id] {
			if (const std::shared_ptr<FRegistryState> Registry = WeakState.lock())
			{
				std::scoped_lock Lock(Registry->Mutex);
				const auto It = Registry->Entries.find(Id);
				if (It != Registry->Entries.end())
				{
					if (It->second.DrawHostPresentation || It->second.StopHostPresentation) Registry->HostPresenters.reset();
					Registry->Entries.erase(It);
				}
			}
		});
	}

	auto CaptureExtensions(EExtensionCategory Category)
		-> std::vector<FExtensionDescriptor>
	{
		const std::shared_ptr<FRegistryState> State = GetRegistryState();
		std::vector<FExtensionDescriptor> Snapshot;
		{
			std::scoped_lock Lock(State->Mutex);
			for (const auto& [Id, Descriptor] : State->Entries)
				if (Descriptor.Category == Category)
					Snapshot.push_back(Descriptor);
		}
		std::ranges::sort(Snapshot, [](const FExtensionDescriptor& Left,
			const FExtensionDescriptor& Right) {
			return std::tie(Left.Order, Left.Id)
				< std::tie(Right.Order, Right.Id);
		});
		return Snapshot;
	}

	auto CaptureHostPresenters() -> std::shared_ptr<const std::vector<FExtensionDescriptor>>
	{
		const std::shared_ptr<FRegistryState> State = GetRegistryState();
		std::scoped_lock Lock(State->Mutex);
		if (State->HostPresenters) return State->HostPresenters;
		auto Snapshot = std::make_shared<std::vector<FExtensionDescriptor>>();
		for (const auto& [Id, Descriptor] : State->Entries)
			if (Descriptor.DrawHostPresentation || Descriptor.StopHostPresentation)
				Snapshot->push_back(Descriptor);
		std::ranges::sort(*Snapshot, [](const FExtensionDescriptor& Left,
			const FExtensionDescriptor& Right) {
			return std::tie(Left.Order, Left.Id)
				< std::tie(Right.Order, Right.Id);
		});
		State->HostPresenters = std::move(Snapshot);
		return State->HostPresenters;
	}

	auto InvokeExtension(
		const FExtensionDescriptor& Descriptor,
		const FExtensionInvocation& Invocation) -> bool
	{
		if (Invocation.bHasHostLifetime && Invocation.HostLifetime.expired()) return false;
		const auto State = GetRegistryState();
		{
			std::scoped_lock Lock(State->Mutex);
			const auto It = State->Entries.find(Descriptor.Id);
			if (It == State->Entries.end() || It->second.RegistrationSerial != Descriptor.RegistrationSerial)
				return false;
		}
		if (!CanInvokeExtension(Descriptor, Invocation.Context, Invocation.bAllowAssetMutation))
			return false;
		return DispatchHostCallback(Invocation.AssetEditors, [&] { Descriptor.Invoke(Invocation); });
	}

	auto CanInvokeExtension(const FExtensionDescriptor& Descriptor,
		const FExtensionContext& Context, bool bAllowContentMutation) -> bool
	{
		return Descriptor.Category != EExtensionCategory::Details
			&& Descriptor.Mutation != EContentMutation::Unspecified
			&& (Descriptor.Mutation == EContentMutation::ReadOnly || bAllowContentMutation)
			&& Descriptor.IsApplicable && Descriptor.Invoke && Descriptor.IsApplicable(Context);
	}

	auto RegisterAssetTypeExtras(FAssetTypeExtras Descriptor, std::string& OutError) -> FScopedExtensionRegistration
	{
		OutError.clear();
		if (Descriptor.AssetClassName.empty() || (!Descriptor.Details && Descriptor.ThumbnailBadgeIcon.empty()))
		{ OutError = "Type extras require a class identity and a details or badge contribution."; return {}; }
		const auto State = GetRegistryState();
		const auto ClassName = Descriptor.AssetClassName;
		{
			std::scoped_lock Lock(State->Mutex);
			if (!State->Extras.emplace(ClassName, std::move(Descriptor)).second)
			{ OutError = "Type extras are already registered."; return {}; }
			++State->PresentationRevision;
		}
		return FScopedExtensionRegistration([Weak = std::weak_ptr(State), ClassName] {
			if (const auto Registry = Weak.lock())
			{ std::scoped_lock Lock(Registry->Mutex); Registry->Extras.erase(ClassName); ++Registry->PresentationRevision; }
		});
	}

	auto FindAssetTypePresentation(std::string_view QualifiedClassName)
		-> std::optional<FAssetTypePresentation>
	{
		std::optional<FAssetTypePresentation> Result;
		const auto State = GetRegistryState();
		{
			std::scoped_lock Lock(State->Mutex);
			if (const auto It = State->Extras.find(std::string(QualifiedClassName)); It != State->Extras.end())
			{
				if (!Result) Result.emplace();
				Result->Details = It->second.Details;
				Result->ThumbnailBadgeIcon = It->second.ThumbnailBadgeIcon;
			}
		}
		if (const auto* Definition = FAssetDefinitionRegistry::Get().FindDefinition(QualifiedClassName))
		{
			const FStrongObjectPtr Lease(const_cast<DAssetDefinition*>(Definition));
			if (!Result) Result.emplace();
			Result->AssetClassName = QualifiedClassName;
			Result->DisplayName = Definition->GetAssetDisplayName();
			Result->Icon = Definition->GetAssetIcon();
			Result->Color = Definition->GetAssetColor();
			Result->CategoryPaths = Definition->GetAssetCategories();
		}
		if (Result) Result->AssetClassName = QualifiedClassName;
		return Result;
	}
	auto CaptureAssetCategoryPaths() -> std::vector<std::vector<std::string>>
	{
		std::vector<std::string> Classes = FAssetDefinitionRegistry::Get().GetRegisteredClassPaths();
		std::set<std::vector<std::string>> Paths;
		for (const auto& Name : Classes)
			if (const auto Type = FindAssetTypePresentation(Name))
				for (const auto& Path : Type->CategoryPaths)
					for (size_t Length = 1; Length <= Path.size(); ++Length)
						Paths.emplace(Path.begin(), Path.begin() + Length);
		return {Paths.begin(), Paths.end()};
	}

	auto GetPresentationRevision() -> uint64
	{
		const auto State = GetRegistryState();
		std::scoped_lock Lock(State->Mutex);
		return State->PresentationRevision + FAssetDefinitionRegistry::Get().GetRevision();
	}
	auto StopHostPresentations(const std::weak_ptr<void>& HostLifetime) -> void
	{
		const auto Snapshot = CaptureHostPresenters();
		const auto State = GetRegistryState();
		for (const auto& Entry : *Snapshot)
		{
			if (!Entry.StopHostPresentation) continue;
			{
				std::scoped_lock Lock(State->Mutex);
				const auto Current = State->Entries.find(Entry.Id);
				if (Current == State->Entries.end() || Current->second.RegistrationSerial != Entry.RegistrationSerial) continue;
			}
			Entry.StopHostPresentation(HostLifetime);
		}
	}

	auto CaptureSelectionDetails(const FExtensionContext& Context) -> std::vector<FDetailRow>
	{
		std::vector<FDetailRow> Rows;
		if (Context.Selection.size() == 1 && Context.Selection.front().Kind == EContentBrowserItemKind::Asset)
			if (const auto Type = FindAssetTypePresentation(Context.Selection.front().AssetClassName);
				Type && Type->Details)
				Rows = Type->Details(Context);
		for (const auto& Extension : CaptureExtensions(EExtensionCategory::Details))
			if (Extension.IsApplicable(Context))
			{
				auto AdditionalRows = Extension.Details(Context);
				Rows.insert(Rows.end(), std::make_move_iterator(AdditionalRows.begin()),
					std::make_move_iterator(AdditionalRows.end()));
			}
		return Rows;
	}

	auto DrawHostPresentation(
		const FExtensionDescriptor& Descriptor,
		bool bAllowAssetMutation, const FExtensionHost& Host) -> bool
	{
		if (!Descriptor.DrawHostPresentation) return false;
		const auto State = GetRegistryState();
		{
			std::scoped_lock Lock(State->Mutex);
			const auto It = State->Entries.find(Descriptor.Id);
			if (It == State->Entries.end() || It->second.RegistrationSerial != Descriptor.RegistrationSerial)
				return false;
		}
		return DispatchHostCallback(Host.AssetEditors, [&] { Descriptor.DrawHostPresentation(Host, bAllowAssetMutation); });
	}
} // namespace Durin::ContentBrowser
