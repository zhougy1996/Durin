#include "DerivedDataBuildExecutionPrivate.h"

namespace Durin::DerivedData
{
	auto Private::FBuildExecutionAccess::Configure(const IBuildFunction& Function) -> FBuildFunctionDescriptor
	{
		FBuildConfigContext Context; Context.Descriptor.Name = std::string(Function.GetName()); Context.Descriptor.Version = Function.GetVersion(); Function.Configure(Context); return std::move(Context.Descriptor);
	}
	auto Private::FBuildExecutionAccess::MakeContext(const FBuildAction& Action, std::span<const FBuildInput> Inputs,
		FBuildOutputBuilder& Output, IRequestOwner& Owner) -> FBuildContext
	{ return FBuildContext(Action, Inputs, Output, Owner); }
	auto Private::FBuildExecutionAccess::MakeOutputBuilder(std::string Schema, uint32 SchemaVersion,
		FBuildOutputLimits Limits) -> FBuildOutputBuilder
	{ return FBuildOutputBuilder(std::move(Schema), SchemaVersion, Limits); }

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
	auto FBuildContext::AddObservation(FValueId Id, FCbObject Object) -> bool { return Output.AddObservation(Id, std::move(Object)); }
	auto FBuildContext::AddMessage(std::string Text) -> bool { return Output.AddMessage(EBuildMessageSeverity::Note, std::move(Text)); }
	auto FBuildContext::AddWarning(std::string Text) -> bool { return Output.AddMessage(EBuildMessageSeverity::Warning, std::move(Text)); }
	auto FBuildContext::AddError(std::string Text) -> bool { return Output.AddMessage(EBuildMessageSeverity::Error, std::move(Text)); }
	auto FBuildContext::FailExecution(std::string Text) -> void
	{ Text.resize(std::min<size_t>(Text.size(), 4096)); ExecutionFailure = std::move(Text); }
	auto FBuildContext::AddLog(std::string Category, EBuildLogSeverity Severity,
		std::string Text) -> bool
	{ return Output.AddLog(std::move(Category), Severity, std::move(Text)); }

	struct Private::FBuildRegistry::FState { std::mutex Mutex; std::vector<std::shared_ptr<const FRegisteredBuildFunction>> Entries; };
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
		if (State->Entries.size() >= 4096) return std::unexpected("Build registry entry limit exceeded.");
		const auto Position = std::ranges::lower_bound(State->Entries, Entry->Descriptor.Name, {}, [](const auto& Item) -> const std::string& { return Item->Descriptor.Name; });
		if (Position != State->Entries.end() && (*Position)->Descriptor.Name == Entry->Descriptor.Name) return std::unexpected("Build function name is already registered.");
		State->Entries.insert(Position, std::move(Entry)); return {};
	}
	catch (const std::exception& E) { return std::unexpected(E.what()); }
	catch (...) { return std::unexpected("Build function registration threw an exception."); }
	auto Private::FBuildRegistry::Freeze() -> FBuildRegistrySnapshot
	{ std::lock_guard Lock(State->Mutex); FBuildRegistrySnapshot Snapshot; Snapshot.Entries = State->Entries; return Snapshot; }
	auto Private::FBuildRegistrySnapshot::Find(std::string_view Name) const -> std::shared_ptr<const FRegisteredBuildFunction>
	{
		const auto Position = std::ranges::lower_bound(Entries, Name, {}, [](const auto& Item) -> const std::string& { return Item->Descriptor.Name; }); return Position != Entries.end() && (*Position)->Descriptor.Name == Name ? *Position : nullptr;
	}
}
