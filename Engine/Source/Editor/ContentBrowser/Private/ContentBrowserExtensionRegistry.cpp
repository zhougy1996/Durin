#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin::Editor::ContentBrowser
{
	namespace
	{
		struct FRegistryState
		{
			std::mutex Mutex;
			std::unordered_map<std::string, FExtensionDescriptor> Entries;
			std::unordered_map<std::string, FAssetTypePresentation> Types;
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
			State->Entries.emplace(Descriptor.Id, std::move(Descriptor));
		}
		return FScopedExtensionRegistration([WeakState = std::weak_ptr(State), Id] {
			if (const std::shared_ptr<FRegistryState> Registry = WeakState.lock())
			{
				std::scoped_lock Lock(Registry->Mutex);
				Registry->Entries.erase(Id);
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

	auto CaptureHostPresenters() -> std::vector<FExtensionDescriptor>
	{
		const std::shared_ptr<FRegistryState> State = GetRegistryState();
		std::vector<FExtensionDescriptor> Snapshot;
		{
			std::scoped_lock Lock(State->Mutex);
			for (const auto& [Id, Descriptor] : State->Entries)
				if (Descriptor.DrawHostPresentation)
					Snapshot.push_back(Descriptor);
		}
		std::ranges::sort(Snapshot, [](const FExtensionDescriptor& Left,
			const FExtensionDescriptor& Right) {
			return std::tie(Left.Order, Left.Id)
				< std::tie(Right.Order, Right.Id);
		});
		return Snapshot;
	}

	auto InvokeExtension(
		const FExtensionDescriptor& Descriptor,
		const FExtensionInvocation& Invocation) -> bool
	{
		const auto State = GetRegistryState();
		{
			std::scoped_lock Lock(State->Mutex);
			const auto It = State->Entries.find(Descriptor.Id);
			if (It == State->Entries.end() || It->second.RegistrationSerial != Descriptor.RegistrationSerial)
				return false;
		}
		if (!CanInvokeExtension(Descriptor, Invocation.Context, Invocation.bAllowAssetMutation))
			return false;
		Descriptor.Invoke(Invocation);
		return true;
	}

	auto CanInvokeExtension(const FExtensionDescriptor& Descriptor,
		const FExtensionContext& Context, bool bAllowContentMutation) -> bool
	{
		return Descriptor.Category != EExtensionCategory::Details
			&& Descriptor.Mutation != EContentMutation::Unspecified
			&& (Descriptor.Mutation == EContentMutation::ReadOnly || bAllowContentMutation)
			&& Descriptor.IsApplicable && Descriptor.Invoke && Descriptor.IsApplicable(Context);
	}

	auto RegisterAssetTypePresentation(FAssetTypePresentation Descriptor, std::string& OutError)
		-> FScopedExtensionRegistration
	{
		OutError.clear();
		if (Descriptor.AssetClassName.empty() || Descriptor.DisplayName.empty() || Descriptor.Icon.empty())
		{
			OutError = "Type presentation requires a qualified class identity, display name and icon.";
			return {};
		}
		const auto State = GetRegistryState();
		const std::string ClassName = Descriptor.AssetClassName;
		{
			std::scoped_lock Lock(State->Mutex);
			if (!State->Types.emplace(ClassName, std::move(Descriptor)).second)
			{
				OutError = std::format("Type presentation '{}' is already registered.", ClassName);
				return {};
			}
			++State->PresentationRevision;
		}
		return FScopedExtensionRegistration([WeakState = std::weak_ptr(State), ClassName] {
			if (const auto Registry = WeakState.lock())
			{
				std::scoped_lock Lock(Registry->Mutex);
				Registry->Types.erase(ClassName);
				++Registry->PresentationRevision;
			}
		});
	}

	auto FindAssetTypePresentation(std::string_view QualifiedClassName)
		-> std::optional<FAssetTypePresentation>
	{
		const auto State = GetRegistryState();
		std::scoped_lock Lock(State->Mutex);
		const auto It = State->Types.find(std::string(QualifiedClassName));
		return It == State->Types.end() ? std::nullopt : std::optional(It->second);
	}

	auto GetPresentationRevision() -> uint64
	{
		const auto State = GetRegistryState();
		std::scoped_lock Lock(State->Mutex);
		return State->PresentationRevision;
	}

	auto CaptureSelectionDetails(const FExtensionContext& Context) -> std::vector<FDetailRow>
	{
		std::vector<FDetailRow> Rows;
		if (Context.Selection.size() == 1 && Context.Selection.front().Kind == EExtensionItemKind::Asset)
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
		bool bAllowAssetMutation) -> bool
	{
		if (!Descriptor.DrawHostPresentation) return false;
		Descriptor.DrawHostPresentation(bAllowAssetMutation);
		return true;
	}
} // namespace Durin::Editor::ContentBrowser
