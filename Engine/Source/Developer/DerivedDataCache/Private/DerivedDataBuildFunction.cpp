#include "DerivedDataBuildExecutionPrivate.h"
#include <mutex>

namespace Durin::DerivedData
{
	FBuildValidationReceipt::~FBuildValidationReceipt() = default;

	struct FBuildInputs::FState
	{
		std::vector<FBuildInputReference> Identities;
		std::shared_ptr<const IBuildInputResolver> Resolver;
	};

	auto Private::FBuildExecutionAccess::Resolve(const FBuildInputs& Inputs, const FBuildCancellation& Cancel)
		-> std::expected<std::vector<FBuildInput>, FBuildFailure>
	{
		if (!Inputs.State || !Inputs.State->Resolver)
			return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InputUnavailable,
				.Operation = EBuildOperation::Resolve, .Description = "Build inputs are unavailable."});
		return Inputs.State->Resolver->Resolve(Inputs.State->Identities, Cancel);
	}

	auto FBuildInputs::TryCreate(std::span<const FBuildSourceReference> Sources,
		std::shared_ptr<const IBuildInputResolver> Resolver, const FBuildCancellation& Cancel)
		-> std::expected<FBuildInputs, FBuildFailure>
	{
		if (!Resolver)
			return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InputUnavailable,
				.Operation = EBuildOperation::Describe, .Description = "Build input resolver is missing."});
		try
		{
			auto Identities = Resolver->Describe(Sources, Cancel);
			if (!Identities)
			{
				auto Failure = std::move(Identities.error());
				Failure.Operation = EBuildOperation::Describe; Failure.BoundDescription();
				return std::unexpected(std::move(Failure));
			}
			if (Cancel.IsCancelled())
				return std::unexpected(FBuildFailure{.Operation = EBuildOperation::Describe,
					.Description = "Build input capture was canceled."});
			std::ranges::sort(*Identities, {}, &FBuildInputReference::Name);
			FBuildInputs Result;
			auto State = std::make_shared<FState>();
			State->Identities = std::move(*Identities);
			State->Resolver = std::move(Resolver);
			Result.State = std::move(State);
			return Result;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::ResourceExhaustion,
				.Operation = EBuildOperation::Describe, .Description = "Allocation"});
		}
		catch (const std::exception& Exception)
		{
			FBuildFailure Failure{.Reason = EBuildFailureReason::InternalFailure,
				.Operation = EBuildOperation::Describe, .Description = Exception.what()};
			Failure.BoundDescription(); return std::unexpected(std::move(Failure));
		}
		catch (...)
		{
			return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InternalFailure,
				.Operation = EBuildOperation::Describe, .Description = "Build input description threw an unknown exception."});
		}
	}

	auto FBuildInputs::GetIdentities() const -> std::span<const FBuildInputReference>
	{ return State ? std::span(State->Identities) : std::span<const FBuildInputReference>{}; }

	struct Private::FBuildRegistry::FState
	{
		std::mutex Mutex;
		bool Frozen = false;
		std::vector<std::shared_ptr<const FRegisteredBuildFunction>> Entries;
	};
	Private::FBuildRegistry::FBuildRegistry() : State(std::make_unique<FState>()) {}
	Private::FBuildRegistry::~FBuildRegistry() = default;

	auto Private::FBuildRegistry::Register(std::shared_ptr<const IBuildFunction> Function)
		-> std::expected<void, FBuildFailure>
	try
	{
		auto Invalid = [](std::string Description) {
			return std::unexpected(FBuildFailure{.Description = std::move(Description)});
		};
		if (!Function) return Invalid("Build function is missing.");
		auto Descriptor = Function->GetDescriptor();
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
		return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::ResourceExhaustion,
			.Description = "Allocation"});
	}
	catch (const std::exception& Exception)
	{
		FBuildFailure Failure{.Reason = EBuildFailureReason::InternalFailure, .Description = Exception.what()};
		Failure.BoundDescription(); return std::unexpected(std::move(Failure));
	}
	catch (...)
	{
		return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InternalFailure,
			.Description = "Build function registration threw an unknown exception."});
	}

	auto Private::FBuildRegistry::Freeze() -> std::expected<FBuildRegistrySnapshot, FBuildFailure>
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
		return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::ResourceExhaustion,
			.Description = "Allocation"});
	}

	auto Private::FBuildRegistrySnapshot::Find(std::string_view Name) const
		-> std::shared_ptr<const FRegisteredBuildFunction>
	{
		const auto Position = std::ranges::lower_bound(Entries, Name,
			{}, [](const auto& Item) -> const std::string& { return Item->Descriptor.Name; });
		return Position != Entries.end() && (*Position)->Descriptor.Name == Name ? *Position : nullptr;
	}
}
