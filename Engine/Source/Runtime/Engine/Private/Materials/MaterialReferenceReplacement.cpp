#include "Materials/Material.h"
#include "MaterialDependencyIndex.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/StrongObjectPtr.h"
#include "Materials/MaterialParameterCollection.h"

namespace Durin
{
	class FMaterialReferenceReplacementParticipant final : public IObjectReplacementParticipant
	{
		struct FSchemaWrite
		{
			TStrongObjectPtr<DMaterial> Owner;
			std::vector<FMaterialParameterDefinition> Before;
			std::vector<FMaterialParameterDefinition> After;
		};
		std::vector<FSchemaWrite> Writes;
		struct FCollectionWrite
		{
			TStrongObjectPtr<DMaterialParameterCollectionSubsystem> Owner;
			FGuid Id;
			DMaterialParameterCollection* Before;
			DMaterialParameterCollection* After;
			uint64 Version;
			size_t StateCount;
			std::shared_ptr<const FMaterialParameterCollectionSnapshot> Snapshot;
		};
		std::vector<FCollectionWrite> CollectionWrites;
		bool bCommitted = false;
		std::optional<Private::FMaterialDependencyIndex> PreparedDependencies;
		uint64 DependencyRevision = 0;
	public:
		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			Writes.clear();
			CollectionWrites.clear();
			bCommitted = false;
			DependencyRevision = Private::GetMaterialDependencyIndex().Revision;
			PreparedDependencies = Private::PrepareMaterialDependencyReplacement(Map);
			for (const auto& Entry : Map.GetEntries())
				if (const auto* Before = Cast<DMaterialParameterCollection>(Entry.Previous))
				{
					const auto* After = Cast<DMaterialParameterCollection>(Entry.Replacement);
					if (!After || Before->GetCollectionId() != After->GetCollectionId() || !After->BuildLayout())
						return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Unsupported});
				}
			for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
			{
				if (!IsValid(Object)) continue;
				if (auto* Subsystem = Cast<DMaterialParameterCollectionSubsystem>(Object))
					for (const auto& [Id, State] : Subsystem->States)
						if (const auto* Entry = Map.Find(State.Collection.Get()))
						{
							auto* Replacement = Cast<DMaterialParameterCollection>(Entry->Replacement);
							if (!Replacement || Replacement->GetCollectionId() != Id || !Replacement->BuildLayout())
								return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Unsupported});
							CollectionWrites.push_back({Subsystem, Id, State.Collection.Get(), Replacement, State.Version, Subsystem->States.size(), State.Snapshot});
						}
				auto* Material = Cast<DMaterial>(Object);
				if (!Material || Map.Find(Material)) continue;
				auto Schema = Material->ParameterSchema;
				bool bChanged = false;
				for (auto& Definition : Schema)
				{
					if (Definition.Type != EMaterialParameterType::Texture) continue;
					auto& Texture = Definition.Value.GetTexture().Texture;
					if (const auto* Entry = Map.Find(Texture.Get()))
					{
						if (Entry->Replacement && !Cast<DTexture2D>(Entry->Replacement))
							return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::Unsupported});
						Texture = Cast<DTexture2D>(Entry->Replacement);
						bChanged = true;
					}
				}
				if (bChanged) Writes.push_back({Material, Material->ParameterSchema, std::move(Schema)});
			}
			return {};
		}
		auto Validate() const -> bool override
		{
			return Private::GetMaterialDependencyIndex().Revision == DependencyRevision && std::ranges::all_of(Writes, [](const auto& Write) {
				return Write.Owner.Get() && Write.Owner->ParameterSchema == Write.Before;
			}) && std::ranges::all_of(CollectionWrites, [](const auto& Write) {
				if (!Write.Owner.Get() || Write.Owner->States.size() != Write.StateCount) return false;
				const auto State = Write.Owner->States.find(Write.Id);
				return State != Write.Owner->States.end() && State->second.Collection.Get() == Write.Before
					&& State->second.Version == Write.Version && State->second.Snapshot == Write.Snapshot;
			});
		}
		auto CoversNativeReferences(const DObject& Owner) const -> bool override
		{
			return std::ranges::any_of(Writes, [&](const auto& Write) { return Write.Owner.Get() == &Owner; })
				|| std::ranges::any_of(CollectionWrites, [&](const auto& Write) { return Write.Owner.Get() == &Owner; });
		}
		auto GetStrongReferenceCount(const DObject&) const -> uint32 override { return 0; }
		auto Commit() noexcept -> void override
		{
			for (auto& Write : Writes) Write.Owner->ParameterSchema.swap(Write.After);
			for (auto& Write : CollectionWrites) Write.Owner->States.find(Write.Id)->second.Collection = Write.After;
			std::swap(Private::GetMaterialDependencyIndex(), *PreparedDependencies);
			bCommitted = true;
		}
		auto Abort() noexcept -> void override { Writes.clear(); CollectionWrites.clear(); PreparedDependencies.reset(); }
		auto CanRetire() const -> bool override { return bCommitted; }
	};

	auto MakeMaterialReferenceReplacementParticipant() -> std::shared_ptr<IObjectReplacementParticipant>
	{
		return std::make_shared<FMaterialReferenceReplacementParticipant>();
	}
}
