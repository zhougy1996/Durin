#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialInterface.h"
#include "Texture/Texture2D.h"
#include "Materials/MaterialParameterValues.h"

#include "MaterialInstance.gen.h"

namespace Durin
{
	enum class EMaterialDynamicParameterUpdateOperation : uint8
	{
		Set,
		Clear,
	};

	// Borrowed by ApplyDynamicParameterUpdates for one GameThread call.
	struct FMaterialDynamicParameterUpdate
	{
		EMaterialDynamicParameterUpdateOperation Operation =
			EMaterialDynamicParameterUpdateOperation::Set;
		FGuid ParameterId;
		FMaterialParameterValue Value;

		static auto Set(FGuid Id, FMaterialParameterValue InValue)
			-> FMaterialDynamicParameterUpdate
		{
			return {.Operation = EMaterialDynamicParameterUpdateOperation::Set,
				.ParameterId = Id, .Value = std::move(InValue)};
		}
		static auto Clear(FGuid Id) -> FMaterialDynamicParameterUpdate
		{
			return {.Operation = EMaterialDynamicParameterUpdateOperation::Clear,
				.ParameterId = Id};
		}
	};

	struct FMaterialDynamicParameterCounters
	{
		uint64 LogicalMutationCount = 0;
		uint64 ChangedCommitCount = 0;
		uint64 NoOpCommitCount = 0;
		uint64 RejectedCommitCount = 0;
		uint64 OwnerPublicationCount = 0;
	};

	ENGINE_API auto GetMaterialDynamicParameterCounters()
		-> FMaterialDynamicParameterCounters;
	// GameThread-only reset; callers must first drain any relevant render work.
	ENGINE_API auto ResetMaterialDynamicParameterCounters() -> void;

	// Resolves inherited material parameters and stores local values by stable identifier.
	DCLASS()
	class DMaterialInstance : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialInstance(const FObjectInitializer& ObjectInitializer);
		// GameThread only. Parent must be a valid non-dynamic material. Outer does
		// not retain this object: callers must keep an ordinary strong reference.
		ENGINE_API static auto CreateDynamic(DMaterialInterface* InParent,
			DObject* Outer = nullptr, FName Name = {}) -> DMaterialInstance*;
		auto IsDynamicInstance() const -> bool override { return bDynamicInstance; }
		ENGINE_API auto GetRenderableStaticProperties() const -> FMaterialStaticProperties override;

		ENGINE_API auto SetParent(DMaterialInterface* InParent) -> bool;
		// Validates and applies a related parent/configuration edit with one request.
		ENGINE_API auto SetParentAndPropertyOverrides(DMaterialInterface* InParent,
			const FMaterialPropertyOverrides& Overrides) -> bool;
		ENGINE_API auto GetParent() const -> DMaterialInterface* override;
		ENGINE_API auto GetStaticProperties() const -> const FMaterialStaticProperties& override;
		ENGINE_API auto GetAcceptedCompiledProgram() const
			-> std::shared_ptr<const FMaterialCompilerResult> override;
		ENGINE_API auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		ENGINE_API auto GetLocalParameterValue(const FGuid& Id, FMaterialParameterValue& OutValue) const -> bool;
		auto GetLocalParameterValueCount() const -> size_t
		{
			return ScalarParameterValues.size() + VectorParameterValues.size() + TextureParameterValues.size();
		}
		// Visits selected values directly from typed storage; no cached alternate record array.
		template<typename TVisitor> auto VisitLocalParameterValues(TVisitor&& Visitor) const -> void
		{
			VisitParameterValueArrays([&](const auto& Records) {
				for (const auto& Record : Records) Visitor(Record.ParameterId, Record.GetValue());
			});
		}
		ENGINE_API auto SetPropertyOverrides(const FMaterialPropertyOverrides& Overrides) -> bool;
		auto GetPropertyOverrides() const -> const FMaterialPropertyOverrides& { return PropertyOverrides; }
		auto GetStaticBoolOverrides() const -> std::span<const FMaterialStaticBoolOverride>
		{
			return StaticBoolOverrides;
		}
		ENGINE_API auto SetStaticBoolOverride(FGuid DeclarationId, bool Value) -> bool;
		ENGINE_API auto ClearStaticBoolOverride(FGuid DeclarationId) -> bool;
		ENGINE_API auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;
		// Authored assets admit edits before compilation; cooked assets use the compiled contract.
		ENGINE_API auto SetParameterValue(
			const FGuid& Id,
			const FMaterialParameterValue& Value
			) -> FMaterialOperationResult;
		// Dynamic instances only. Validates the complete borrowed batch against one
		// accepted parent contract for Set. Clear only needs a valid local GUID,
		// including orphaned values. Commits and publishes at most once.
		ENGINE_API auto ApplyDynamicParameterUpdates(
			std::span<const FMaterialDynamicParameterUpdate> Updates)
			-> FMaterialOperationResult;
		ENGINE_API auto ClearParameterValue(const FGuid& Id) -> bool;
		ENGINE_API auto HasLocalParameterValue(const FGuid& Id) const -> bool;
		ENGINE_API auto IsParameterValueOrphan(const FGuid& Id) const -> bool;
		ENGINE_API auto SetScalarParameterValue(FName Name, float Value) -> FMaterialOperationResult;
		ENGINE_API auto SetVector2ParameterValue(FName Name, const FVector2& Value) -> FMaterialOperationResult;
		ENGINE_API auto SetVectorParameterValue(FName Name, const FVector3& Value) -> FMaterialOperationResult;
		ENGINE_API auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> FMaterialOperationResult;
		ENGINE_API auto ClearScalarParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearVector2ParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearVectorParameterValue(FName Name) -> bool;
		ENGINE_API auto ClearTextureParameterValue(FName Name) -> bool;
		ENGINE_API auto HasLocalScalarParameterValue(FName Name) const -> bool;
		ENGINE_API auto HasLocalVector2ParameterValue(FName Name) const -> bool;
		ENGINE_API auto HasLocalVectorParameterValue(FName Name) const -> bool;
		ENGINE_API auto HasLocalTextureParameterValue(FName Name) const -> bool;
		ENGINE_API auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		ENGINE_API auto GetVector2ParameterValue(FName Name, FVector2& OutValue) const -> bool override;
		ENGINE_API auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		ENGINE_API auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool override;
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		// Factory-only lifecycle, never serialized or switched on a live asset.
		bool bDynamicInstance = false;

		DPROPERTY(Edit)
		TObjectPtr<DMaterialInterface> Parent;

		DPROPERTY(Edit)
		std::vector<FMaterialScalarParameterValue> ScalarParameterValues;

		DPROPERTY(Edit)
		std::vector<FMaterialVectorParameterValue> VectorParameterValues;

		DPROPERTY(Edit)
		std::vector<FMaterialTextureParameterValue> TextureParameterValues;

		auto ValidateParameterStorage(const FPropertyEditProposal* Proposal = nullptr) const -> FMaterialOperationResult;
		template<typename TVisitor> auto VisitParameterValueArrays(TVisitor&& Visitor) -> void
		{
			Visitor(ScalarParameterValues);
			Visitor(VectorParameterValues);
			Visitor(TextureParameterValues);
		}
		template<typename TVisitor> auto VisitParameterValueArrays(TVisitor&& Visitor) const -> void
		{
			Visitor(ScalarParameterValues);
			Visitor(VectorParameterValues);
			Visitor(TextureParameterValues);
		}

		DPROPERTY(Edit)
		FMaterialPropertyOverrides PropertyOverrides;

		DPROPERTY(Edit)
		std::vector<FMaterialStaticBoolOverride> StaticBoolOverrides;

		mutable FMaterialStaticProperties ResolvedStaticProperties;
	};
}
