#include "DerivedDataBuildFunction.h"
#include <mutex>

namespace Durin::DerivedData
{
	struct FBuildRegistry::FState
	{
		std::mutex Mutex;
		bool Frozen = false;
		std::vector<std::shared_ptr<const FRegisteredBuildFunction>> Entries;
	};

	FBuildRegistry::FBuildRegistry() : State(std::make_unique<FState>()) {}
	FBuildRegistry::~FBuildRegistry() = default;

	auto FBuildRegistry::Register(std::shared_ptr<const IBuildFunction> Function) -> std::expected<void, FBuildError>
	try
	{
		auto Invalid = [](std::string Description) {
			return std::unexpected(FBuildError{.Description = std::move(Description)});
		};
		if (!Function) return Invalid("Build function is missing.");
		auto Descriptor = Function->GetDescriptor(); // No registry lock across producer code.
		auto Definition = FBuildDefinition::TryCreate(Descriptor.Name, {}, {});
		if (!Definition || !FBuildAction::TryCreate(*Definition, Descriptor, {})
			|| !FBuildOutput::TryCreate({.Schema = Descriptor.OutputType, .SchemaVersion = Descriptor.OutputSchema}))
			return Invalid("Build function descriptor is invalid.");
		auto Entry = std::make_shared<const FRegisteredBuildFunction>(std::move(Descriptor), std::move(Function));
		std::lock_guard Lock(State->Mutex);
		if (State->Frozen) return Invalid("Build registry is frozen.");
		if (State->Entries.size() >= 4096) return Invalid("Build registry entry limit exceeded.");
		const auto Position = std::ranges::lower_bound(State->Entries, Entry->Descriptor.Name,
			{}, [](const auto& Item) -> const std::string& { return Item->Descriptor.Name; });
		if (Position != State->Entries.end() && (*Position)->Descriptor.Name == Entry->Descriptor.Name)
			return Invalid("Build function name is already registered.");
		State->Entries.insert(Position, std::move(Entry));
		return {};
	}
	catch (const std::bad_alloc&)
	{
		return std::unexpected(FBuildError{.Category = EBuildErrorCategory::Unavailable, .Description = "Allocation"});
	}

	auto FBuildRegistry::Freeze() -> std::expected<FBuildRegistrySnapshot, FBuildError>
	try
	{
		std::lock_guard Lock(State->Mutex);
		FBuildRegistrySnapshot Snapshot;
		Snapshot.Entries = State->Entries;
		State->Frozen = true;
		return Snapshot;
	}
	catch (const std::bad_alloc&)
	{
		return std::unexpected(FBuildError{.Category = EBuildErrorCategory::Unavailable, .Description = "Allocation"});
	}

	auto FBuildRegistrySnapshot::Find(std::string_view Name) const -> std::shared_ptr<const FRegisteredBuildFunction>
	{
		const auto Position = std::ranges::lower_bound(Entries, Name,
			{}, [](const auto& Item) -> const std::string& { return Item->Descriptor.Name; });
		return Position != Entries.end() && (*Position)->Descriptor.Name == Name ? *Position : nullptr;
	}
}
