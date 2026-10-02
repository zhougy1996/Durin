#include "Materials/Material.h"
#include "MaterialDependencyIndex.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/StrongObjectPtr.h"

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
		bool bCommitted = false;
		std::optional<Private::FMaterialDependencyIndex> PreparedDependencies;
		uint64 DependencyRevision = 0;
	public:
		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			Writes.clear();
			bCommitted = false;
			DependencyRevision = Private::GetMaterialDependencyIndex().Revision;
			PreparedDependencies = Private::PrepareMaterialDependencyReplacement(Map);
			for (auto* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeUnpublished))
			{
				if (!IsValid(Object)) continue;
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
			});
		}
		auto CoversNativeReferences(const DObject& Owner) const -> bool override
		{
			return std::ranges::any_of(Writes, [&](const auto& Write) { return Write.Owner.Get() == &Owner; });
		}
		auto GetStrongReferenceCount(const DObject&) const -> uint32 override { return 0; }
		auto Commit() noexcept -> void override
		{
			for (auto& Write : Writes) Write.Owner->ParameterSchema.swap(Write.After);
			std::swap(Private::GetMaterialDependencyIndex(), *PreparedDependencies);
			bCommitted = true;
		}
		auto Abort() noexcept -> void override { Writes.clear(); PreparedDependencies.reset(); }
		auto CanRetire() const -> bool override { return bCommitted; }
	};

	auto MakeMaterialReferenceReplacementParticipant() -> std::shared_ptr<IObjectReplacementParticipant>
	{
		return std::make_shared<FMaterialReferenceReplacementParticipant>();
	}
}
