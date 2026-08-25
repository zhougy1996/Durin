#include "ContentBrowser/ContentBrowserContracts.h"

namespace Durin::Editor::ContentBrowser
{
	namespace
	{
		struct FRegistryState
		{
			std::mutex Mutex;
			std::unordered_map<std::string, FExtensionDescriptor> Entries;
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
		if (Descriptor.Id.empty() || Descriptor.Label.empty() || !Descriptor.IsApplicable
			|| !Descriptor.Invoke || !Descriptor.OwnerGate.IsValid())
		{
			OutError = "A Content Browser extension requires an ID, label, callbacks, and a live owner gate.";
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

	auto InvokeExtension(
		const FExtensionDescriptor& Descriptor,
		const FExtensionInvocation& Invocation) -> bool
	{
		if (!Descriptor.IsApplicable || !Descriptor.Invoke
			|| !Descriptor.IsApplicable(Invocation.Context))
			return false;
		FModuleOwnedCallbackInvocation Admission =
			Descriptor.OwnerGate.TryEnter();
		if (!Admission) return false;
		Descriptor.Invoke(Invocation);
		return true;
	}
} // namespace Durin::Editor::ContentBrowser
