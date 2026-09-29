#include "DerivedDataBuildExecutionPrivate.h"
#include <mutex>

namespace Durin::DerivedData
{
	struct FBuildInputs::FState { std::vector<FBuildInputReference> Identities; std::shared_ptr<const IBuildInputResolver> Resolver; };
	auto Private::FBuildExecutionAccess::Resolve(const FBuildInputs& Inputs, const FBuildCancellation& Cancel)
		-> std::expected<std::vector<FBuildInput>, FBuildInputError>
	{
		if (!Inputs.State || !Inputs.State->Resolver) return std::unexpected(FBuildInputError{"Build inputs are unavailable."});
		return Inputs.State->Resolver->Resolve(Inputs.State->Identities, Cancel);
	}
	auto Private::FBuildExecutionAccess::Configure(const IBuildFunction& Function) -> FBuildFunctionDescriptor
	{
		FBuildConfigContext Context; Context.Descriptor.Name = std::string(Function.GetName()); Context.Descriptor.Version = Function.GetVersion(); Function.Configure(Context); return std::move(Context.Descriptor);
	}
	auto Private::FBuildExecutionAccess::MakeContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs,
		FBuildOutputBuilder& Output, FBuildCancellation Cancel, FBuildMetricSink Metrics, uint64 MaximumWorkingSetBytes) -> FBuildContext
	{ return FBuildContext(Action, Inputs, Output, std::move(Cancel), std::move(Metrics), MaximumWorkingSetBytes); }
	auto Private::FBuildExecutionAccess::MakeOutputBuilder(std::string Schema, uint32 SchemaVersion,
		FBuildOutputLimits Limits) -> FBuildOutputBuilder
	{ return FBuildOutputBuilder(std::move(Schema), SchemaVersion, Limits); }

	auto FBuildInputs::TryCreate(std::span<const FBuildSourceReference> Sources,
		std::shared_ptr<const IBuildInputResolver> Resolver, const FBuildCancellation& Cancel)
		-> std::expected<FBuildInputs, FBuildInputError>
	try
	{
		if (!Resolver) return std::unexpected(FBuildInputError{"Build input resolver is missing."});
		auto Identities = Resolver->Describe(Sources, Cancel); if (!Identities) return std::unexpected(std::move(Identities.error()));
		if (Cancel.IsCancelled()) return std::unexpected(FBuildInputError{"Build input capture was canceled."});
		std::ranges::sort(*Identities, {}, &FBuildInputReference::Name);
		FBuildInputs Result; auto State = std::make_shared<FState>(); State->Identities = std::move(*Identities); State->Resolver = std::move(Resolver); Result.State = std::move(State); return Result;
	}
	catch (const std::exception& E) { return std::unexpected(FBuildInputError{E.what()}); }
	catch (...) { return std::unexpected(FBuildInputError{"Build input description threw an exception."}); }
	auto FBuildInputs::GetIdentities() const -> std::span<const FBuildInputReference> { return State ? std::span(State->Identities) : std::span<const FBuildInputReference>{}; }
	auto FBuildInputsBuilder::Build() && -> std::expected<FBuildInputs, FBuildInputError>
	{ return FBuildInputs::TryCreate(Sources, std::move(Resolver), Cancel); }

	auto FBuildContext::FindConstant(std::string_view Name) const -> const FBuildConstantValue*
	{
		const auto Values = Action.GetConstants(); const auto It = std::ranges::lower_bound(Values, Name, {}, &FBuildConstant::Name); return It != Values.end() && It->Name == Name ? &It->Value : nullptr;
	}
	auto FBuildContext::FindInput(std::string_view Name) const -> const FBuildInput*
	{
		const auto It = std::ranges::lower_bound(Inputs, Name, {}, [](const FBuildInput& Input) -> const std::string& { return Input.Identity.Name; }); return It != Inputs.end() && It->Identity.Name == Name ? &*It : nullptr;
	}
	auto FBuildContext::AddValue(FValueId Id, FSharedByteBuffer Data) -> bool { return Output.AddValue(Id, std::move(Data)); }
	auto FBuildContext::AddMeta(FValueId Id, FCbObject Object) -> bool { return Output.AddMeta(Id, std::move(Object)); }
	auto FBuildContext::AddMessage(std::string Text) -> bool { return Output.AddMessage(EBuildMessageSeverity::Note, std::move(Text)); }
	auto FBuildContext::AddWarning(std::string Text) -> bool { return Output.AddMessage(EBuildMessageSeverity::Warning, std::move(Text)); }
	auto FBuildContext::AddError(std::string Text) -> bool { return Output.AddMessage(EBuildMessageSeverity::Error, std::move(Text)); }

	struct Private::FBuildRegistry::FState { std::mutex Mutex; bool Frozen = false; std::vector<std::shared_ptr<const FRegisteredBuildFunction>> Entries; };
	Private::FBuildRegistry::FBuildRegistry() : State(std::make_unique<FState>()) {}
	Private::FBuildRegistry::~FBuildRegistry() = default;
	auto Private::FBuildRegistry::Register(std::shared_ptr<const IBuildFunction> Function) -> std::expected<void, std::string>
	try
	{
		if (!Function) return std::unexpected("Build function is missing.");
		auto Descriptor = FBuildExecutionAccess::Configure(*Function);
		auto Definition = std::move(FBuildDefinitionBuilder(Descriptor.Name)).Build();
		if (!Definition || !std::move(FBuildActionBuilder(*Definition, Descriptor)).Build()) return std::unexpected("Build function descriptor is invalid.");
		auto Entry = std::make_shared<const FRegisteredBuildFunction>(std::move(Descriptor), std::move(Function)); std::lock_guard Lock(State->Mutex);
		if (State->Frozen) return std::unexpected("Build registry is frozen."); if (State->Entries.size() >= 4096) return std::unexpected("Build registry entry limit exceeded.");
		const auto Position = std::ranges::lower_bound(State->Entries, Entry->Descriptor.Name, {}, [](const auto& Item) -> const std::string& { return Item->Descriptor.Name; });
		if (Position != State->Entries.end() && (*Position)->Descriptor.Name == Entry->Descriptor.Name) return std::unexpected("Build function name is already registered.");
		State->Entries.insert(Position, std::move(Entry)); return {};
	}
	catch (const std::exception& E) { return std::unexpected(E.what()); }
	catch (...) { return std::unexpected("Build function registration threw an exception."); }
	auto Private::FBuildRegistry::Freeze() -> std::expected<FBuildRegistrySnapshot, std::string>
	try { std::lock_guard Lock(State->Mutex); FBuildRegistrySnapshot Snapshot; Snapshot.Entries = State->Entries; State->Frozen = true; return Snapshot; }
	catch (...) { return std::unexpected("Build registry snapshot allocation failed."); }
	auto Private::FBuildRegistrySnapshot::Find(std::string_view Name) const -> std::shared_ptr<const FRegisteredBuildFunction>
	{
		const auto Position = std::ranges::lower_bound(Entries, Name, {}, [](const auto& Item) -> const std::string& { return Item->Descriptor.Name; }); return Position != Entries.end() && (*Position)->Descriptor.Name == Name ? *Position : nullptr;
	}
}
