#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "Serialization/Archive.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Durin
{
	class FProperty;
	class FPropertyValueSnapshot;

	enum class EArchiveObjectReferenceKind : uint8 { Null, Internal, External };

	struct FArchiveLogicalTypeDescriptor
	{
		enum class EKind : uint8
		{
			Scalar, Enum, String, Name, Guid, Bytes, BulkData, Object, SoftObject, WeakObject,
			Struct, Array, Map, FixedArray
		};

		EKind Kind = EKind::Bytes;
		bool bSigned = false;
		bool bFloating = false;
		uint8 BitWidth = 0;
		FName QualifiedType;
		uint32 NativeFieldVersion = 0;
		uint32 FixedArrayDimension = 0;
		std::shared_ptr<FArchiveLogicalTypeDescriptor> ElementType;
		std::shared_ptr<FArchiveLogicalTypeDescriptor> KeyType;
		std::shared_ptr<FArchiveLogicalTypeDescriptor> ValueType;

		COREDOBJECT_API static auto Scalar(bool bSigned, uint8 BitWidth, bool bFloating = false) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Enum(FName QualifiedType, bool bSigned, uint8 BitWidth) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto String() -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Name() -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Guid() -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Bytes() -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto BulkData() -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Object(FName QualifiedType = {}) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto SoftObject(FName QualifiedType = {}) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto WeakObject(FName QualifiedType = {}) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Struct(FName QualifiedType, uint32 NativeFieldVersion = 0) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Array(FArchiveLogicalTypeDescriptor Element) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto Map(FArchiveLogicalTypeDescriptor Key, FArchiveLogicalTypeDescriptor Value) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto FixedArray(FArchiveLogicalTypeDescriptor Element, uint32 Dimension) -> FArchiveLogicalTypeDescriptor;
	};

	struct FArchiveFieldDescriptor
	{
		FName DeclaringType;
		FName Name;
		FArchiveLogicalTypeDescriptor LogicalType;
		uint32 ArrayDimension = 1;
		EPropertyFlags PropertyFlags = EPropertyFlags::None;
	};

	class FObjectArchive;
	class FArchivePathScope
	{
	public:
		FArchivePathScope() = default;
		FArchivePathScope(const FArchivePathScope&) = delete;
		auto operator=(const FArchivePathScope&) -> FArchivePathScope& = delete;
		COREDOBJECT_API FArchivePathScope(FArchivePathScope&& Other) noexcept;
		COREDOBJECT_API auto operator=(FArchivePathScope&& Other) noexcept -> FArchivePathScope&;
		COREDOBJECT_API ~FArchivePathScope();
	private:
		explicit FArchivePathScope(FObjectArchive* InArchive) : Archive(InArchive) {}
		FObjectArchive* Archive = nullptr;
		friend class FObjectArchive;
	};

	class FArchiveObjectScope
	{
	public:
		FArchiveObjectScope() = default;
		FArchiveObjectScope(const FArchiveObjectScope&) = delete;
		auto operator=(const FArchiveObjectScope&) -> FArchiveObjectScope& = delete;
		COREDOBJECT_API FArchiveObjectScope(FArchiveObjectScope&& Other) noexcept;
		COREDOBJECT_API auto operator=(FArchiveObjectScope&& Other) noexcept -> FArchiveObjectScope&;
		COREDOBJECT_API ~FArchiveObjectScope();
	private:
		explicit FArchiveObjectScope(FObjectArchive* InArchive) : Archive(InArchive) {}
		FObjectArchive* Archive = nullptr;
		friend class FObjectArchive;
	};

	class FArchiveFieldScope
	{
	public:
		FArchiveFieldScope() = default;
		FArchiveFieldScope(const FArchiveFieldScope&) = delete;
		auto operator=(const FArchiveFieldScope&) -> FArchiveFieldScope& = delete;
		COREDOBJECT_API FArchiveFieldScope(FArchiveFieldScope&& Other) noexcept;
		COREDOBJECT_API auto operator=(FArchiveFieldScope&& Other) noexcept -> FArchiveFieldScope&;
		COREDOBJECT_API ~FArchiveFieldScope();
	private:
		explicit FArchiveFieldScope(FObjectArchive* InArchive) : Archive(InArchive) {}
		FObjectArchive* Archive = nullptr;
		friend class FObjectArchive;
	};

	// Adds reflected fields and object-reference semantics over the Core byte Archive.
	class FObjectArchive : public FArchive
	{
	public:
		COREDOBJECT_API explicit FObjectArchive(FArchiveState State, FArchiveVersionContext Versions = {});

		COREDOBJECT_API auto EnterObject(DObject& Object) -> FArchiveObjectScope;
		COREDOBJECT_API auto EnterField(const FArchiveFieldDescriptor& Field) -> FArchiveFieldScope;
		COREDOBJECT_API auto EnterFixedArrayElement(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto EnterArrayElement(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto EnterMapKey(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto EnterMapValue(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto NotifyCanonicalMapKey(
			uint64 Index, std::span<const uint8> Token) -> void;
		COREDOBJECT_API auto MarkBaseReflectedFieldsSerialized() -> void;
		COREDOBJECT_API auto NotifyReflectedPropertyValue(
			FProperty& Property,
			const void* Container,
			uint32 ArrayIndex) -> void;

		virtual COREDOBJECT_API auto SerializeObjectReference(DObject*& Value) -> void;
		virtual COREDOBJECT_API auto SerializeSoftObjectPath(FSoftObjectPath& Value) -> void;
		virtual COREDOBJECT_API auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void;

	protected:
		virtual COREDOBJECT_API auto OnEnterObject(DObject& Object) -> void;
		virtual COREDOBJECT_API auto OnLeaveObject() -> void;
		virtual COREDOBJECT_API auto OnEnterField(const FArchiveFieldDescriptor& Field) -> void;
		virtual COREDOBJECT_API auto OnLeaveField() -> void;
		virtual COREDOBJECT_API auto OnEnterFixedArrayElement(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnEnterArrayElement(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnEnterMapKey(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnEnterMapValue(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnCanonicalMapKey(
			uint64 Index, std::span<const uint8> Token) -> void;
		virtual COREDOBJECT_API auto OnLeavePath() -> void;
		virtual COREDOBJECT_API auto OnReflectedPropertyValue(
			FProperty& Property,
			const void* Container,
			uint32 ArrayIndex) -> void;

	private:
		struct FObjectScopeState
		{
			bool bBaseMarked = false;
			uint32 ActiveFieldDepth = 0;
			std::unordered_set<std::string> Fields;
		};

		COREDOBJECT_API auto CloseObjectScope() -> void;
		COREDOBJECT_API auto CloseFieldScope() -> void;
		COREDOBJECT_API auto ClosePathScope() -> void;
		std::vector<FObjectScopeState> ObjectScopes;
		std::vector<bool> FieldScopes;

		friend class FArchiveObjectScope;
		friend class FArchiveFieldScope;
		friend class FArchivePathScope;
	};

	// Object-aware canonical memory writer used by graph and reflection adapters.
	class FObjectMemoryWriter : public FObjectArchive
	{
	public:
		COREDOBJECT_API explicit FObjectMemoryWriter(
			std::vector<uint8>& InBytes,
			EArchivePurpose Purpose = EArchivePurpose::ObjectGraph);
		COREDOBJECT_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
		auto Tell() const -> uint64 override { return static_cast<uint64>(Bytes.size()); }
	private:
		std::vector<uint8>& Bytes;
	};

	// Object-aware canonical memory reader used by graph and reflection adapters.
	class FObjectMemoryReader : public FObjectArchive
	{
	public:
		COREDOBJECT_API explicit FObjectMemoryReader(
			std::span<const uint8> InBytes,
			EArchivePurpose Purpose = EArchivePurpose::ObjectGraph);
		COREDOBJECT_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
		auto GetRemainingPayloadBytes() const -> uint64 override
		{
			return static_cast<uint64>(Bytes.size()) - Offset;
		}
		auto Tell() const -> uint64 override { return Offset; }
	private:
		std::span<const uint8> Bytes;
		uint64 Offset = 0;
	};

	// Transitional source aliases; new persistent byte code includes Core's
	// canonical archives directly, while existing object consumers migrate by stage.
	using FMemoryWriter = FObjectMemoryWriter;
	using FMemoryReader = FObjectMemoryReader;

	COREDOBJECT_API auto RequireObjectArchive(FArchive& Ar) -> FObjectArchive*;
	COREDOBJECT_API auto EnterArchiveObject(FArchive& Ar, DObject& Object) -> FArchiveObjectScope;
	COREDOBJECT_API auto EnterArchiveField(
		FArchive& Ar, const FArchiveFieldDescriptor& Field) -> FArchiveFieldScope;
	COREDOBJECT_API auto EnterArchiveFixedArrayElement(FArchive& Ar, uint64 Index) -> FArchivePathScope;
	COREDOBJECT_API auto EnterArchiveArrayElement(FArchive& Ar, uint64 Index) -> FArchivePathScope;
	COREDOBJECT_API auto EnterArchiveMapKey(FArchive& Ar, uint64 Index) -> FArchivePathScope;
	COREDOBJECT_API auto EnterArchiveMapValue(FArchive& Ar, uint64 Index) -> FArchivePathScope;
	COREDOBJECT_API auto NotifyArchiveCanonicalMapKey(
		FArchive& Ar, uint64 Index, std::span<const uint8> Token) -> void;
	COREDOBJECT_API auto MarkArchiveBaseReflectedFieldsSerialized(FArchive& Ar) -> void;
	COREDOBJECT_API auto NotifyArchiveReflectedPropertyValue(
		FArchive& Ar, FProperty& Property, const void* Container, uint32 ArrayIndex) -> void;
	COREDOBJECT_API auto SerializeArchiveObjectReference(FArchive& Ar, DObject*& Value) -> void;
	COREDOBJECT_API auto SerializeArchiveSoftObjectPath(FArchive& Ar, FSoftObjectPath& Value) -> void;
	COREDOBJECT_API auto SerializeArchiveWeakObjectReference(FArchive& Ar, FWeakObjectPtr& Value) -> void;

	class FPropertyValueSnapshot
	{
	public:
		FPropertyValueSnapshot() = default;
		COREDOBJECT_API ~FPropertyValueSnapshot();
		COREDOBJECT_API FPropertyValueSnapshot(const FPropertyValueSnapshot& Other);
		COREDOBJECT_API auto operator=(const FPropertyValueSnapshot& Other) -> FPropertyValueSnapshot&;
		COREDOBJECT_API FPropertyValueSnapshot(FPropertyValueSnapshot&& Other) noexcept;
		COREDOBJECT_API auto operator=(FPropertyValueSnapshot&& Other) noexcept -> FPropertyValueSnapshot&;
		auto IsValid() const -> bool { return Property != nullptr; }
		auto GetProperty() const -> const FProperty* { return Property; }
		auto GetBytes() const -> const std::vector<uint8>& { return Bytes; }
		auto GetReferencedObjects() const -> const std::vector<DObject*>& { return ReferencedObjects; }
		COREDOBJECT_API auto operator==(const FPropertyValueSnapshot& Other) const -> bool;
	private:
		const FProperty* Property = nullptr;
		std::vector<uint8> Bytes;
		std::vector<DObject*> ReferencedObjects;
		auto AddReferenceRoots() -> void;
		auto ReleaseReferenceRoots() -> void;
		friend COREDOBJECT_API auto CapturePropertyValue(const FProperty*, const void*, uint32, FPropertyValueSnapshot&, std::string*) -> bool;
		friend COREDOBJECT_API auto RestorePropertyValue(const FProperty*, void*, uint32, const FPropertyValueSnapshot&, std::string*) -> bool;
	};

	COREDOBJECT_API auto CapturePropertyValue(const FProperty* Property, const void* Container, uint32 ArrayIndex, FPropertyValueSnapshot& OutSnapshot, std::string* OutError = nullptr) -> bool;
	COREDOBJECT_API auto RestorePropertyValue(const FProperty* Property, void* Container, uint32 ArrayIndex, const FPropertyValueSnapshot& Snapshot, std::string* OutError = nullptr) -> bool;
	COREDOBJECT_API auto SerializeReflectedPropertyValue(FArchive& Ar, FProperty& Property, void* Container, uint32 ArrayIndex = 0, bool bIncludeRawObjectReferences = false) -> void;
	COREDOBJECT_API auto SerializeDObjectProperties(FArchive& Ar, DObject& Object) -> void;
	COREDOBJECT_API auto SaveObjectGraphToMemory(DObject* RootObject, std::vector<uint8>& OutBytes) -> bool;
	COREDOBJECT_API auto LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes) -> DObject*;
	COREDOBJECT_API auto DuplicateObjectGraph(DObject* RootObject, DObject* NewOuter, FName NewName = FName(), std::string* OutError = nullptr, std::unordered_map<DObject*, DObject*>* OutDuplicates = nullptr) -> DObject*;
	COREDOBJECT_API auto CopyEditableObjectProperties(DObject* Source, DObject* Destination, const std::unordered_map<DObject*, DObject*>& ReferenceMap, std::string* OutError = nullptr) -> bool;
}
