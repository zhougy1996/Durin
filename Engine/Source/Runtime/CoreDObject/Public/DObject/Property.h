#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectGlobals.h"
#include "Field.h"

namespace Durin
{
	// Runtime-owned immutable view of selected first-party property metadata.
	struct FPropertyMetadata
	{
		std::string DisplayName;
		std::string ToolTip;
		std::string Category;
		EPropertyUnit Units = EPropertyUnit::None;
		FPropertyMetadataNumber Step;
		int8 Precision = -1;
		FPropertyMetadataNumber ClampMin;
		FPropertyMetadataNumber ClampMax;
		FPropertyMetadataNumber UIMin;
		FPropertyMetadataNumber UIMax;
	};

	struct FPropertyDeprecation
	{
		FGuid CustomVersionGuid;
		int32 DeprecatedBefore = 0;
		int32 LatestVersion = 0;
		FName HistoricalName;
		std::vector<FName> MigrationTargets;
	};

	class FDefaultObjectGraphMap;
	class FStructProperty;
	// Describes one reflected field's storage, flags, referenced type, and value lifecycle.
	class FProperty : public FField
	{
		DECLARE_FIELD(FProperty, FField, EClassCastFlags::FProperty, COREDOBJECT_API)
	public:
		COREDOBJECT_API FProperty(FFieldVariant InOwner, FName InName, EObjectFlags InObjectFlags);

		COREDOBJECT_API FProperty(
			FFieldVariant InOwner,
			FName InName,
			EObjectFlags InObjectFlags,
			EPropertyFlags InPropertyFlags,
			uint16 InArrayDim,
			uint16 InOffset,
			uint16 InElementSize,
			DurinCodeGen::EPropertyGenFlags InKind,
			DClass* InReferencedClass,
			bool bInIsObjectPtrWrapper = false
		);

		auto GetPropertyFlags() const -> EPropertyFlags { return PropertyFlags; }
		auto GetArrayDim() const -> uint16 { return ArrayDim; }
		auto GetOffset() const -> uint16 { return Offset; }
		auto GetElementSize() const -> uint16 { return ElementSize; }
		auto GetKind() const -> DurinCodeGen::EPropertyGenFlags { return Kind; }
		auto GetReferencedClass() const -> DClass* { return ReferencedClass; }
		auto IsObjectPtrWrapper() const -> bool { return bIsObjectPtrWrapper; }
		auto GetLegacyNames() const -> std::span<const FName> { return LegacyNames; }
		auto GetTypedMetadata() const -> const FPropertyMetadata& { return TypedMetadata; }
		COREDOBJECT_API auto SetTypedMetadata(const FPropertyMetadataParams* InMetadata) -> void;
		auto IsDeprecated() const -> bool { return Deprecation.has_value(); }
		auto GetDeprecation() const -> const FPropertyDeprecation* { return Deprecation ? &*Deprecation : nullptr; }
		COREDOBJECT_API auto SetDeprecation(const FPropertyDeprecationParams* InDeprecation) -> void;
		auto MatchesSerializedName(FName InName) const -> bool
		{
			return NamePrivate == InName || std::ranges::find(LegacyNames, InName) != LegacyNames.end();
		}
		COREDOBJECT_API auto SetLegacyNames(std::span<const char* const> InLegacyNames) -> void;
		COREDOBJECT_API auto GetValueSize() const -> uint32;
		COREDOBJECT_API auto GetValueAlignment() const -> uint32;
		COREDOBJECT_API auto CanDefaultConstructValue() const -> bool;
		COREDOBJECT_API auto CanDestroyValue() const -> bool;
		COREDOBJECT_API auto CanCopyConstructValue() const -> bool;
		COREDOBJECT_API auto CanCopyAssignValue() const -> bool;
		auto HasValueLifecycle() const -> bool { return CanDefaultConstructValue() && CanDestroyValue(); }
		auto HasValueAccessors() const -> bool { return MutableValueAccessor != nullptr || ConstValueAccessor != nullptr; }
		auto GetOwnerProperty() const -> FProperty* { return static_cast<FProperty*>(Owner.ToField()); }
		auto HasAnyPropertyFlags(EPropertyFlags InFlags) const -> bool { return EnumHasAnyFlags(PropertyFlags, InFlags); }

		auto GetValuePtr(void* Container, uint32 ArrayIndex = 0) const -> void*
		{
			if (MutableValueAccessor) return MutableValueAccessor(Container, ArrayIndex);
			return static_cast<std::byte*>(Container) + Offset + ElementSize * ArrayIndex;
		}

		auto GetValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const void*
		{
			if (ConstValueAccessor) return ConstValueAccessor(Container, ArrayIndex);
			return static_cast<const std::byte*>(Container) + Offset + ElementSize * ArrayIndex;
		}

		auto SetValueAccessors(
			void* (*InMutableAccessor)(void*, uint32),
			const void* (*InConstAccessor)(const void*, uint32)
		) -> void
		{
			MutableValueAccessor = InMutableAccessor;
			ConstValueAccessor = InConstAccessor;
		}

		auto SetValueLifecycle(
			uint32 InValueSize,
			uint32 InValueAlignment,
			void (*InInitializeValue)(void*),
			void (*InDestroyValue)(void*),
			void (*InCopyConstructValue)(void*, const void*) = nullptr,
			void (*InCopyAssignValue)(void*, const void*) = nullptr
		) -> void
		{
			ValueSize = InValueSize;
			ValueAlignment = InValueAlignment;
			InitializeValueFunction = InInitializeValue;
			DestroyValueFunction = InDestroyValue;
			CopyConstructValueFunction = InCopyConstructValue;
			CopyAssignValueFunction = InCopyAssignValue;
		}

		auto SetBulkDataOperations(
			void (*InSerializeValue)(FArchive&, void*),
			bool (*InIdenticalValue)(const void*, const void*)) -> void
		{
			SerializeBulkDataValueFunction = InSerializeValue;
			IdenticalBulkDataValueFunction = InIdenticalValue;
		}
		auto SerializeBulkDataValue(FArchive& Ar, void* Value) const -> bool
		{
			if (!SerializeBulkDataValueFunction) return false;
			SerializeBulkDataValueFunction(Ar, Value);
			return true;
		}
		auto AreBulkDataValuesIdentical(const void* Left, const void* Right) const -> std::optional<bool>
		{
			if (!IdenticalBulkDataValueFunction) return std::nullopt;
			return IdenticalBulkDataValueFunction(Left, Right);
		}

		COREDOBJECT_API auto InitializeValue(void* Memory, std::string* OutError = nullptr) const -> bool;
		COREDOBJECT_API auto DestroyValue(void* Memory) const -> void;
		COREDOBJECT_API auto CopyConstructValue(
			void* Destination, const void* Source, std::string* OutError = nullptr) const -> bool;
		COREDOBJECT_API auto CopyAssignValue(
			void* Destination, const void* Source, std::string* OutError = nullptr) const -> bool;

		template<typename T>
		auto ContainerPtrToValuePtr(void* Container, uint32 ArrayIndex = 0) const -> T*
		{
			return static_cast<T*>(GetValuePtr(Container, ArrayIndex));
		}

		template<typename T>
		auto ContainerPtrToValuePtr(const void* Container, uint32 ArrayIndex = 0) const -> const T*
		{
			return static_cast<const T*>(GetValuePtr(Container, ArrayIndex));
		}

	private:
		EPropertyFlags PropertyFlags = EPropertyFlags::None;
		uint16 ArrayDim = 1;
		uint16 Offset = 0;
		uint16 ElementSize = 0;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		DClass* ReferencedClass = nullptr;
		bool bIsObjectPtrWrapper = false;
		std::vector<FName> LegacyNames;
		FPropertyMetadata TypedMetadata;
		std::optional<FPropertyDeprecation> Deprecation;
		uint32 ValueSize = 0;
		uint32 ValueAlignment = 0;
		void (*InitializeValueFunction)(void*) = nullptr;
		void (*DestroyValueFunction)(void*) = nullptr;
		void (*CopyConstructValueFunction)(void*, const void*) = nullptr;
		void (*CopyAssignValueFunction)(void*, const void*) = nullptr;
		void (*SerializeBulkDataValueFunction)(FArchive&, void*) = nullptr;
		bool (*IdenticalBulkDataValueFunction)(const void*, const void*) = nullptr;
		void* (*MutableValueAccessor)(void*, uint32) = nullptr;
		const void* (*ConstValueAccessor)(const void*, uint32) = nullptr;
	};

	// Owns one fully constructed reflected value in aligned detached storage.
	// Struct lifetimes and copy modes are always dispatched through FDStructOps.
	class FReflectedValueStorage
	{
	public:
		FReflectedValueStorage() = default;
		COREDOBJECT_API ~FReflectedValueStorage();
		FReflectedValueStorage(const FReflectedValueStorage&) = delete;
		auto operator=(const FReflectedValueStorage&) -> FReflectedValueStorage& = delete;
		COREDOBJECT_API FReflectedValueStorage(FReflectedValueStorage&& Other) noexcept;
		COREDOBJECT_API auto operator=(FReflectedValueStorage&& Other) noexcept -> FReflectedValueStorage&;

		COREDOBJECT_API auto DefaultConstruct(
			const FProperty* InProperty, uint32 InArrayIndex = 0, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API auto CopyConstruct(
			const FProperty* InProperty, const void* SourceValue,
			uint32 InArrayIndex = 0, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API auto CopyAssign(const void* SourceValue, std::string* OutError = nullptr) -> bool;
		COREDOBJECT_API auto Reset() -> void;

		auto IsLive() const -> bool { return bLive; }
		auto GetProperty() const -> const FProperty* { return Property; }
		auto GetContainer() const -> void* { return Memory; }
		auto GetValue() const -> void* { return Value; }
		auto GetArrayIndex() const -> uint32 { return ArrayIndex; }

	private:
		auto Allocate(const FProperty* InProperty, uint32 InArrayIndex, std::string* OutError) -> bool;
		auto Fail(std::string* OutError, std::string_view Operation) const -> bool;

		const FProperty* Property = nullptr;
		uint32 ArrayIndex = 0;
		void* Memory = nullptr;
		void* Value = nullptr;
		size_t Alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
		bool bLive = false;
	};

	// Validates an editor-authored value against hard typed metadata without modifying it.
	COREDOBJECT_API auto ValidatePropertyEditValue(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex = 0,
		std::string* OutError = nullptr
	) -> bool;

	// Distinguishes an authored value difference from unavailable comparison semantics.
	enum class EPropertyIdentityResult : uint8
	{
		Identical,
		Different,
		Unsupported,
	};

	// Stable reasons reported by the bounded logical identity walk.
	enum class EPropertyIdentityReason : uint8
	{
		None,
		ValueMismatch,
		ContainerLengthMismatch,
		MapKeyMissing,
		InvalidInput,
		InvalidArrayIndex,
		UnsupportedLogicalKind,
		MissingStructDescriptor,
		IncompleteAuthoredFields,
		MissingArrayDescriptor,
		MissingArrayOperations,
		MissingMapDescriptor,
		MissingMapOperations,
		UnsupportedMapKey,
		ContainerOperationFailed,
		DescriptorCycle,
		DepthLimit,
		DiagnosticPathLimit,
	};

	inline constexpr uint32 PropertyIdentityMaxDepth = 64;
	inline constexpr size_t PropertyIdentityMaxPathLength = 1024;

	struct FPropertyIdentityDiagnostic
	{
		std::string PropertyPath;
		DurinCodeGen::EPropertyGenFlags LogicalKind = DurinCodeGen::EPropertyGenFlags::None;
		EPropertyIdentityReason Reason = EPropertyIdentityReason::None;

		auto Reset() -> void
		{
			PropertyPath.clear();
			LogicalKind = DurinCodeGen::EPropertyGenFlags::None;
			Reason = EPropertyIdentityReason::None;
		}
	};

	COREDOBJECT_API auto ComparePropertyValues(
		const FProperty* Property,
		const void* LeftContainer,
		uint32 LeftArrayIndex,
		const void* RightContainer,
		uint32 RightArrayIndex,
		FPropertyIdentityDiagnostic* OutDiagnostic = nullptr) -> EPropertyIdentityResult;

	COREDOBJECT_API auto ComparePropertyValuesWithDefaultGraph(
		const FProperty* Property,
		const void* LeftContainer,
		uint32 LeftArrayIndex,
		const void* RightContainer,
		uint32 RightArrayIndex,
		const FDefaultObjectGraphMap& DefaultGraph,
		FPropertyIdentityDiagnostic* OutDiagnostic = nullptr) -> EPropertyIdentityResult;

	COREDOBJECT_API auto CompareObjectPropertyToClassDefault(
		const FProperty* Property,
		const DObject* LiveObject,
		uint32 ArrayIndex,
		const FDefaultObjectGraphMap& DefaultGraph,
		FPropertyIdentityDiagnostic* OutDiagnostic = nullptr) -> EPropertyIdentityResult;

	COREDOBJECT_API auto CompareStructPropertyToTypeDefault(
		const FStructProperty* Property,
		const void* LiveContainer,
		uint32 ArrayIndex,
		FPropertyIdentityDiagnostic* OutDiagnostic = nullptr) -> EPropertyIdentityResult;

	COREDOBJECT_API auto CompareStructValues(
		const DStruct* Struct,
		const void* LeftValue,
		const void* RightValue,
		FPropertyIdentityDiagnostic* OutDiagnostic = nullptr) -> EPropertyIdentityResult;

	COREDOBJECT_API auto ValidatePropertyIdentityDescriptor(
		const FProperty* Property,
		FPropertyIdentityDiagnostic* OutDiagnostic = nullptr) -> bool;

	COREDOBJECT_API auto ArePropertyValuesIdentical(
		const FProperty* Property,
		const void* LeftContainer,
		uint32 LeftArrayIndex,
		const void* RightContainer,
		uint32 RightArrayIndex) -> bool;
}
