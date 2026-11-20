#include "DerivedDataBuildExecutionPrivate.h"
#include "DerivedDataBuildFunctionFactory.h"
#include "Logging/LogMacros.h"

namespace Durin::DerivedData
{
	auto Private::FBuildExecutionAccess::Configure(const IBuildFunction& Function) -> FBuildFunctionDescriptor
	{
		FBuildConfigContext Context; Context.Descriptor.Name = std::string(Function.GetName()); Context.Descriptor.Version = Function.GetVersion(); Function.Configure(Context); return std::move(Context.Descriptor);
	}
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

	namespace
	{
		constexpr size_t MaximumBuildFunctions = 4096;

		auto MakeRegisteredBuildFunction(std::shared_ptr<const IBuildFunction> Function)
			-> std::expected<std::shared_ptr<const Private::FRegisteredBuildFunction>, std::string>
		try
		{
			if (!Function) return std::unexpected("Build function is missing.");
			auto Descriptor = Private::FBuildExecutionAccess::Configure(*Function);
			if (!Private::ValidateBuildFunction(Descriptor)) return std::unexpected("Build function descriptor is invalid.");
			return std::make_shared<const Private::FRegisteredBuildFunction>(std::move(Descriptor), std::move(Function));
		}
		catch (const std::exception& E) { return std::unexpected(E.what()); }
		catch (...) { return std::unexpected("Build function registration threw an exception."); }

		constexpr auto GetBuildFunctionName = [](const auto& Entry) -> const std::string& { return Entry->Descriptor.Name; };
	}

	struct Private::FBuildRegistry::FState
	{
		std::mutex Mutex;
		FBuildRegistrySnapshot::FEntries Entries;
		FBuildRegistrySnapshot Snapshot;
	};
	Private::FBuildRegistry::FBuildRegistry() : State(std::make_unique<FState>()) {}
	Private::FBuildRegistry::~FBuildRegistry() = default;
	auto Private::FBuildRegistry::Register(std::shared_ptr<const IBuildFunction> Function) -> std::expected<void, std::string>
	try
	{
		auto Entry = MakeRegisteredBuildFunction(std::move(Function));
		if (!Entry) return std::unexpected(std::move(Entry.error()));
		std::lock_guard Lock(State->Mutex);
		if (State->Entries.size() >= MaximumBuildFunctions) return std::unexpected("Build registry entry limit exceeded.");
		const auto& Name = GetBuildFunctionName(*Entry);
		const auto Position = std::ranges::lower_bound(State->Entries, Name, {}, GetBuildFunctionName);
		if (Position != State->Entries.end() && GetBuildFunctionName(*Position) == Name) return std::unexpected("Build function name is already registered.");
		State->Entries.insert(Position, std::move(*Entry));
		State->Snapshot = {};
		return {};
	}
	catch (const std::exception& E) { return std::unexpected(E.what()); }
	catch (...) { return std::unexpected("Build function registration threw an exception."); }
	auto Private::FBuildRegistry::Freeze() -> FBuildRegistrySnapshot
	{
		std::lock_guard Lock(State->Mutex);
		if (!State->Snapshot.Entries && !State->Entries.empty())
			State->Snapshot.Entries = std::make_shared<const FBuildRegistrySnapshot::FEntries>(State->Entries);
		return State->Snapshot;
	}
	auto Private::FBuildRegistrySnapshot::Find(std::string_view Name) const -> std::shared_ptr<const FRegisteredBuildFunction>
	{
		if (!Entries) return nullptr;
		const auto Position = std::ranges::lower_bound(*Entries, Name, {}, GetBuildFunctionName);
		return Position != Entries->end() && GetBuildFunctionName(*Position) == Name ? *Position : nullptr;
	}

	namespace Private
	{
		class FBuildFunctionRegistry
		{
		public:
			FBuildFunctionRegistry()
			{
				auto& Features = IModularFeatures::Get();
				IModularFeatures::FScopedLockModularFeatureList Lock;
				const auto Factories = Features.GetModularFeatureImplementations<IModularFeature>(FactoryType);
				for (auto* Factory : Factories)
					AddFactory(Factory);
				Publish(Factories);
				Features.OnModularFeatureRegistered().AddRaw(this, &FBuildFunctionRegistry::OnFactoryChanged);
				try
				{
					Features.OnModularFeatureUnregistered().AddRaw(this, &FBuildFunctionRegistry::OnFactoryChanged);
				}
				catch (...)
				{
					Features.OnModularFeatureRegistered().RemoveAll(this);
					throw;
				}
			}

			~FBuildFunctionRegistry()
			{
				auto& Features = IModularFeatures::Get();
				IModularFeatures::FScopedLockModularFeatureList Lock;
				Features.OnModularFeatureRegistered().RemoveAll(this);
				Features.OnModularFeatureUnregistered().RemoveAll(this);
			}

			auto GetSnapshot() const -> Private::FBuildRegistrySnapshot
			{
				std::lock_guard Lock(Mutex);
				return Snapshot;
			}

		private:
			auto OnFactoryChanged(const FName& Type, IModularFeature* Factory) -> void
			{
				if (Type != FactoryType) return;

				// Notifications are synchronous under the feature list lock. Check
				// membership before borrowing the pointer because another subscriber
				// may have changed the list in a nested notification.
				const auto Factories = IModularFeatures::Get().GetModularFeatureImplementations<IModularFeature>(FactoryType);
				if (std::ranges::find(Factories, Factory) != Factories.end())
				{
					if (Candidates.contains(Factory)) return;
					AddFactory(Factory);
				}
				else if (Candidates.erase(Factory) == 0)
				{
					return;
				}
				Publish(Factories);
			}

			auto AddFactory(IModularFeature* Factory) -> void
			{
				auto Entry = MakeRegisteredBuildFunction(static_cast<const IBuildFunctionFactory*>(Factory)->GetFunction());
				if (!Entry)
				{
					Candidates.emplace(Factory, nullptr);
					DURIN_ERROR_CATEGORY("DerivedData", "Build function factory rejected: {}", Entry.error());
					return;
				}
				const auto& Name = GetBuildFunctionName(*Entry);
				if (std::ranges::any_of(Candidates, [&](const auto& Candidate) {
					return Candidate.second && GetBuildFunctionName(Candidate.second) == Name;
				}))
				{
					DURIN_ERROR_CATEGORY("DerivedData", "Build function factory for '{}' is shadowed by an earlier registration.", Name);
				}
				Candidates.emplace(Factory, std::move(*Entry));
			}

			auto Publish(std::span<IModularFeature* const> Factories) -> void
			{
				FBuildRegistrySnapshot::FEntries Entries;
				std::unordered_set<std::string_view> Names;
				for (auto* Factory : Factories)
				{
					const auto It = Candidates.find(Factory);
					if (It == Candidates.end() || !It->second) continue;
					if (Names.insert(GetBuildFunctionName(It->second)).second)
						Entries.push_back(It->second);
					if (Entries.size() == MaximumBuildFunctions) break;
				}
				std::ranges::sort(Entries, {}, GetBuildFunctionName);
				if (Snapshot.Entries ? *Snapshot.Entries == Entries : Entries.empty()) return;

				FBuildRegistrySnapshot Next;
				if (!Entries.empty())
					Next.Entries = std::make_shared<const FBuildRegistrySnapshot::FEntries>(std::move(Entries));
				std::lock_guard Lock(Mutex);
				std::swap(Snapshot, Next);
			}

			const FName FactoryType{IBuildFunctionFactory::FeatureName};
			std::unordered_map<const IModularFeature*, std::shared_ptr<const FRegisteredBuildFunction>> Candidates;
			mutable std::mutex Mutex;
			Private::FBuildRegistrySnapshot Snapshot;
		};
	}

	auto Private::GetBuildFunctionRegistrySnapshot() -> FBuildRegistrySnapshot
	{
		static Private::FBuildFunctionRegistry Registry;
		return Registry.GetSnapshot();
	}
}
