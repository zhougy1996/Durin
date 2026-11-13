#include "AssetDefinition/AssetDefinitionRegistry.h"

#include "AssetDefinition/AssetDefinition.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Package.h"
#include "Modules/ModuleManager.h"
#include "Threading/RunnableThread.h"

namespace Durin::AssetDefinitionPrivate
{
	struct FRegistryEntry
	{
		FObjectKey Definition;
		FStrongObjectPtr OwnedDefinition;
		uint64 Id = 0;
		FName Provider;
	};

	struct FRegistryState
	{
		std::unordered_map<std::string, FRegistryEntry> Entries;
		uint64 NextId = 1;
		uint64 Revision = 1;
	};

	auto Remove(FRegistryState& State, uint64 Id) -> void
	{
		require(IsInGameThread());
		if (std::erase_if(State.Entries, [Id](const auto& Pair) { return Pair.second.Id == Id; }))
			++State.Revision;
	}

	auto Insert(FRegistryState& State, DAssetDefinition& Definition, bool bOwn,
		std::string* OutError) -> uint64
	{
		require(IsInGameThread());
		if (OutError) OutError->clear();
		const std::string ClassPath = Definition.GetAssetClassPath();
		if (!IsValid(&Definition) || ClassPath.empty())
		{
			if (OutError) *OutError = "A live definition and a nonempty asset class path are required.";
			return 0;
		}
		if (const auto Existing = State.Entries.find(ClassPath); Existing != State.Entries.end())
		{
			if (Existing->second.Definition.ResolveObjectPtr())
			{
				if (OutError) *OutError = std::format("Asset class '{}' already has a definition.", ClassPath);
				return 0;
			}
			State.Entries.erase(Existing);
		}
		const uint64 Id = State.NextId++;
		const auto* Package = Definition.GetClass()->GetPackage();
		State.Entries.emplace(ClassPath, FRegistryEntry{
			.Definition = FObjectKey(&Definition),
			.OwnedDefinition = bOwn ? FStrongObjectPtr(&Definition) : FStrongObjectPtr{},
			.Id = Id, .Provider = Package && Package->IsCppPackage() ? Package->GetFName() : FName{}});
		++State.Revision;
		return Id;
	}
}

namespace Durin
{
	FAssetDefinitionRegistration::~FAssetDefinitionRegistration() { Reset(); }
	FAssetDefinitionRegistration::FAssetDefinitionRegistration(FAssetDefinitionRegistration&& Other) noexcept
		: State(std::move(Other.State)), Id(std::exchange(Other.Id, 0)) {}
	auto FAssetDefinitionRegistration::operator=(FAssetDefinitionRegistration&& Other) noexcept -> FAssetDefinitionRegistration&
	{
		if (this != &Other)
		{
			Reset();
			State = std::move(Other.State);
			Id = std::exchange(Other.Id, 0);
		}
		return *this;
	}
	auto FAssetDefinitionRegistration::Reset() -> void
	{
		if (auto Pinned = State.lock(); Pinned && Id) AssetDefinitionPrivate::Remove(*Pinned, Id);
		State.reset();
		Id = 0;
	}

	FAssetDefinitionRegistry::FAssetDefinitionRegistry()
		: State(std::make_shared<AssetDefinitionPrivate::FRegistryState>()) {}
	FAssetDefinitionRegistry::~FAssetDefinitionRegistry() = default;
	auto FAssetDefinitionRegistry::Get() -> FAssetDefinitionRegistry&
	{
		static FAssetDefinitionRegistry Registry;
		return Registry;
	}
	auto FAssetDefinitionRegistry::GetAssetDefinitionVersion() const -> uint64 { return State->Revision; }
	auto FAssetDefinitionRegistry::GetRegisteredClassPaths() const -> std::vector<std::string>
	{
		require(IsInGameThread());
		std::vector<std::string> Result;
		for (const auto& [Path, Entry] : State->Entries)
			if (Entry.Definition.ResolveObjectPtr()) Result.push_back(Path);
		std::ranges::sort(Result);
		return Result;
	}
	auto FAssetDefinitionRegistry::RegisterAssetDefinition(DAssetDefinition& Definition,
		std::string* OutError) -> FAssetDefinitionRegistration
	{
		FAssetDefinitionRegistration Registration;
		Registration.Id = AssetDefinitionPrivate::Insert(*State, Definition, true, OutError);
		if (Registration.Id) Registration.State = State;
		return Registration;
	}
	auto FAssetDefinitionRegistry::RegisterClassDefaultObject(DAssetDefinition& Definition,
		std::string* OutError) -> bool
	{
		if (!Definition.IsClassDefaultObject() || !Definition.CanRegisterStatically())
		{
			if (OutError) *OutError = "Only an eligible class-default object can register automatically.";
			return false;
		}
		return AssetDefinitionPrivate::Insert(*State, Definition, false, OutError) != 0;
	}
	auto FAssetDefinitionRegistry::UnregisterAssetDefinition(FObjectKey Definition) -> void
	{
		require(IsInGameThread());
		if (std::erase_if(State->Entries, [&](const auto& Pair) { return Pair.second.Definition == Definition; }))
			++State->Revision;
	}
	auto FAssetDefinitionRegistry::GetAssetDefinitionForClass(const DClass* AssetClass) -> const DAssetDefinition*
	{ return AssetClass ? GetAssetDefinitionForClass(AssetClass->GetQualifiedName().ToString()) : nullptr; }
	auto FAssetDefinitionRegistry::GetAssetDefinitionForClass(std::string_view AssetClassPath) -> const DAssetDefinition*
	{
		require(IsInGameThread());
		// No ancestor cache: late derived definitions and removals take effect immediately.
		std::string Path(AssetClassPath);
		DClass* Class = FindClassByQualifiedName(FName(Path));
		for (;;)
		{
			if (const auto Entry = State->Entries.find(Path); Entry != State->Entries.end())
			{
				if (!Entry->second.Provider.IsNone())
				{
					const auto Provider = FModuleManager::Get().FindModule(Entry->second.Provider);
					if (Provider && Provider->State.load() != EModuleState::Active) return nullptr;
				}
				if (auto* Definition = Cast<DAssetDefinition>(Entry->second.Definition.ResolveObjectPtr()); IsValid(Definition))
					return Definition;
				State->Entries.erase(Entry);
				++State->Revision;
			}
			Class = Class ? Class->GetSuperClass() : nullptr;
			if (!Class) return nullptr;
			Path = Class->GetQualifiedName().ToString();
		}
	}
}
