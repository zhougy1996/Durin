#pragma once

#include "CoreMinimal.h"

#include "DObject/Object.h"
#include "Engine/WorldSubsystem.h"
#include "Materials/MaterialCompiledLayout.h"

#include "MaterialParameterCollection.gen.h"

namespace Durin
{
	class FSceneInterface;

	inline constexpr uint32 MaterialParameterCollectionMaxDeclarationCount = 128;
	inline constexpr uint32 MaterialParameterCollectionMaxPerMaterial = 4;
	inline constexpr uint32 CurrentMaterialParameterCollectionSchemaVersion = 1;

	DSTRUCT()
	struct FMaterialParameterCollectionDeclaration
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid Id;

		DPROPERTY(Edit)
		FName Name;

		DPROPERTY(Edit)
		EMaterialParameterType Type = EMaterialParameterType::Scalar;

		// Only the components selected by Type participate in the default payload.
		DPROPERTY(Edit)
		FVector4 DefaultValue{0.0};

		DPROPERTY(Edit)
		std::string DisplayName;

		DPROPERTY(Edit)
		FName GroupName;

		DPROPERTY(Edit)
		int32 SortOrder = 0;

		auto operator==(const FMaterialParameterCollectionDeclaration&) const -> bool = default;
	};

	enum class EMaterialParameterCollectionError : uint8
	{
		None,
		InvalidCollection,
		InvalidDeclaration,
		DuplicateId,
		DuplicateName,
		UnsupportedType,
		NonFiniteValue,
		DeclarationLimit,
		DeclarationNotFound,
		DuplicateUpdate,
		TypeMismatch,
		InvalidState,
	};

	struct FMaterialParameterCollectionResult
	{
		EMaterialParameterCollectionError Error = EMaterialParameterCollectionError::None;
		uint32 RecordIndex = 0;
		FGuid ParameterId;
		std::string Message;
		explicit operator bool() const { return Error == EMaterialParameterCollectionError::None; }
	};

	struct FMaterialParameterCollectionLayout
	{
		// Exact top-level asset object path for Cook/runtime default resolution.
		// Transient compiler fixtures may leave this empty.
		std::string AssetPath;
		FGuid CollectionId;
		FMaterialRenderLayout UniformLayout;
		// Runtime fallback data; intentionally excluded from shader identity.
		FByteBuffer DefaultPayload;
		uint32 SchemaVersion = CurrentMaterialParameterCollectionSchemaVersion;

		auto HasCompatibleSchema(const FMaterialParameterCollectionLayout& Other) const -> bool
		{
			return CollectionId == Other.CollectionId
				&& UniformLayout == Other.UniformLayout
				&& SchemaVersion == Other.SchemaVersion;
		}
		auto operator==(const FMaterialParameterCollectionLayout&) const -> bool = default;
	};

	struct FMaterialParameterCollectionSnapshot
	{
		FMaterialParameterCollectionLayout Layout;
		uint64 Version = 0;
		FByteBuffer Payload;
	};

	DCLASS()
	class DMaterialParameterCollection : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialParameterCollection(const FObjectInitializer& Initializer);
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto PostEditChangeProperty(
			const FPropertyChangedEvent& Event) -> void override;

		auto GetCollectionId() const -> FGuid { return CollectionId; }
		auto GetDeclarations() const -> std::span<const FMaterialParameterCollectionDeclaration>
		{
			return Declarations;
		}
		auto GetSchemaRevision() const -> uint64 { return SchemaRevision; }
		auto GetDefaultsRevision() const -> uint64 { return DefaultsRevision; }

		[[nodiscard]] ENGINE_API auto Validate() const -> FMaterialParameterCollectionResult;
		[[nodiscard]] ENGINE_API auto BuildLayout() const
			-> std::expected<FMaterialParameterCollectionLayout, FMaterialParameterCollectionResult>;
		[[nodiscard]] ENGINE_API auto BuildDefaultPayload() const
			-> std::expected<FByteBuffer, FMaterialParameterCollectionResult>;
		[[nodiscard]] ENGINE_API auto SetDeclarations(
			std::span<const FMaterialParameterCollectionDeclaration> InDeclarations)
			-> FMaterialParameterCollectionResult;
		[[nodiscard]] ENGINE_API auto SetDefaultValue(
			FGuid ParameterId, const FVector4& Value) -> FMaterialParameterCollectionResult;
		ENGINE_API auto FindDeclaration(FGuid ParameterId) const
			-> const FMaterialParameterCollectionDeclaration*;
		ENGINE_API auto FindDeclaration(FName Name) const
			-> const FMaterialParameterCollectionDeclaration*;

	private:
		DPROPERTY()
		FGuid CollectionId;

		DPROPERTY(Edit)
		std::vector<FMaterialParameterCollectionDeclaration> Declarations;

		DPROPERTY()
		uint32 SchemaVersion = CurrentMaterialParameterCollectionSchemaVersion;

		uint64 SchemaRevision = 1;
		uint64 DefaultsRevision = 1;
		std::vector<FMaterialParameterCollectionDeclaration> LastValidDeclarations;
		auto CommitDeclarationMutation(
			std::span<const FMaterialParameterCollectionDeclaration> Previous) -> void;
	};

	// Refreshes loaded material owners that captured this collection. Schema edits
	// request a new identity; default-only edits reuse the existing shader artifact.
	ENGINE_API auto NotifyMaterialParameterCollectionChanged(
		DMaterialParameterCollection& Collection) -> void;

	enum class EMaterialParameterCollectionUpdateOperation : uint8 { Set, Clear };

	struct FMaterialParameterCollectionUpdate
	{
		EMaterialParameterCollectionUpdateOperation Operation =
			EMaterialParameterCollectionUpdateOperation::Set;
		FGuid ParameterId;
		FVector4 Value{0.0};

		static auto Set(FGuid Id, const FVector4& InValue)
			-> FMaterialParameterCollectionUpdate
		{
			return {.Operation = EMaterialParameterCollectionUpdateOperation::Set,
				.ParameterId = Id, .Value = InValue};
		}
		static auto Clear(FGuid Id) -> FMaterialParameterCollectionUpdate
		{
			return {.Operation = EMaterialParameterCollectionUpdateOperation::Clear,
				.ParameterId = Id};
		}
	};

	struct FMaterialParameterCollectionCounters
	{
		uint64 SubmittedRecords = 0;
		uint64 ChangedCommits = 0;
		uint64 NoOpCommits = 0;
		uint64 RejectedCommits = 0;
		uint64 Publications = 0;
		uint64 AssetRefreshPublications = 0;
		uint64 PayloadBytes = 0;
	};

	DCLASS(NoClassDefaultObject)
	class DMaterialParameterCollectionSubsystem : public DWorldSubsystem
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialParameterCollectionSubsystem(
			const FObjectInitializer& Initializer);
		ENGINE_API auto ApplyUpdates(DMaterialParameterCollection& Collection,
			std::span<const FMaterialParameterCollectionUpdate> Updates)
			-> FMaterialParameterCollectionResult;
		ENGINE_API auto SetValue(DMaterialParameterCollection& Collection,
			FGuid ParameterId, const FVector4& Value)
			-> FMaterialParameterCollectionResult;
		ENGINE_API auto SetValue(DMaterialParameterCollection& Collection,
			FName ParameterName, const FVector4& Value)
			-> FMaterialParameterCollectionResult;
		ENGINE_API auto ClearValue(DMaterialParameterCollection& Collection,
			FGuid ParameterId) -> FMaterialParameterCollectionResult;
		ENGINE_API auto ClearValue(DMaterialParameterCollection& Collection,
			FName ParameterName) -> FMaterialParameterCollectionResult;
		ENGINE_API auto GetValue(const DMaterialParameterCollection& Collection,
			FGuid ParameterId, FVector4& OutValue) const -> bool;
		ENGINE_API auto GetValue(const DMaterialParameterCollection& Collection,
			FName ParameterName, FVector4& OutValue) const -> bool;
		ENGINE_API auto GetSnapshot(const DMaterialParameterCollection& Collection) const
			-> std::shared_ptr<const FMaterialParameterCollectionSnapshot>;
		// Refreshes an already-instantiated world state after an authored schema or
		// default edit. Unreferenced collections remain lazy.
		ENGINE_API auto RefreshCollection(DMaterialParameterCollection& Collection)
			-> FMaterialParameterCollectionResult;
		auto GetCounters() const -> FMaterialParameterCollectionCounters { return Counters; }
		ENGINE_API auto AddReferencedObjects(FReferenceCollector& Collector) -> void override;
		ENGINE_API auto Deinitialize() noexcept -> void override;
		ENGINE_API auto OnRenderSceneChanged(FSceneInterface* Previous,
			FSceneInterface* Current) noexcept -> void override;

	private:
		struct FState
		{
			TObjectPtr<DMaterialParameterCollection> Collection;
			FMaterialParameterCollectionLayout Layout;
			std::unordered_map<FGuid, FVector4> Overrides;
			std::shared_ptr<const FMaterialParameterCollectionSnapshot> Snapshot;
			uint64 Version = 0;
		};
		ENGINE_API auto FindOrCreateState(DMaterialParameterCollection& Collection)
			-> std::expected<FState*, FMaterialParameterCollectionResult>;
		ENGINE_API auto BuildSnapshot(const FState& State,
			const std::unordered_map<FGuid, FVector4>& Overrides, uint64 Version) const
			-> std::expected<std::shared_ptr<const FMaterialParameterCollectionSnapshot>,
				FMaterialParameterCollectionResult>;
		ENGINE_API auto Publish(FState& State,
			std::unordered_map<FGuid, FVector4> Overrides)
			-> FMaterialParameterCollectionResult;

		std::unordered_map<FGuid, FState> States;
		FMaterialParameterCollectionCounters Counters;
	};
}
