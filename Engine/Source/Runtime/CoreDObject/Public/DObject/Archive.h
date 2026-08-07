#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "Misc/Guid.h"
#include "Misc/Name.h"

#include <concepts>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace Durin
{
	class FProperty;
	class FPropertyValueSnapshot;

	enum class EArchiveDirection : uint8 { Load, Save };
	enum class EArchivePurpose : uint8
	{
		Discovery,
		ObjectGraph,
		Duplicate,
		PropertySnapshot,
		EditableCopy,
		AuthoredPackage
	};
	enum class EArchiveCapability : uint32
	{
		None = 0,
		StructuredFields = 1 << 0,
		RawBytes = 1 << 1,
		CanonicalMapOrder = 1 << 2,
		ObjectReferences = 1 << 3,
		SoftObjectReferences = 1 << 4,
		UnknownFieldRetention = 1 << 5,
		RemainingPayload = 1 << 6,
		CustomVersions = 1 << 7,
		MultiPassDiscovery = 1 << 8
	};

	constexpr auto operator|(EArchiveCapability Left, EArchiveCapability Right) -> EArchiveCapability
	{
		return static_cast<EArchiveCapability>(static_cast<uint32>(Left) | static_cast<uint32>(Right));
	}
	constexpr auto operator&(EArchiveCapability Left, EArchiveCapability Right) -> EArchiveCapability
	{
		return static_cast<EArchiveCapability>(static_cast<uint32>(Left) & static_cast<uint32>(Right));
	}
	constexpr auto operator|=(EArchiveCapability& Left, EArchiveCapability Right) -> EArchiveCapability&
	{
		Left = Left | Right;
		return Left;
	}

	enum class EArchiveObjectReferenceKind : uint8 { Null, Internal, External };

	struct FArchiveState
	{
		EArchiveDirection Direction = EArchiveDirection::Save;
		EArchivePurpose Purpose = EArchivePurpose::ObjectGraph;
		EArchiveCapability Capabilities = EArchiveCapability::None;
	};

	struct FArchiveLogicalTypeDescriptor
	{
		enum class EKind : uint8
		{
			Scalar, Enum, String, Name, Guid, Bytes, Object, SoftObject,
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
		COREDOBJECT_API static auto Object(FName QualifiedType = {}) -> FArchiveLogicalTypeDescriptor;
		COREDOBJECT_API static auto SoftObject(FName QualifiedType = {}) -> FArchiveLogicalTypeDescriptor;
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

	struct FArchiveFormatVersion { FName Format; uint32 Version = 0; };
	struct FArchiveCustomVersion { FGuid Key; int32 Version = 0; };
	struct FArchiveVersionContext
	{
		std::vector<FArchiveFormatVersion> Formats;
		std::vector<FArchiveCustomVersion> CustomVersions;
		COREDOBJECT_API auto FindFormat(FName Format) const -> const FArchiveFormatVersion*;
		COREDOBJECT_API auto FindCustom(const FGuid& Key) const -> const FArchiveCustomVersion*;
	};

	enum class EArchiveFailureCode : uint8
	{
		UnsupportedCapability,
		UnsupportedType,
		InvalidData,
		TruncatedPayload,
		UnbalancedScope,
		MissingBaseReflectedFields,
		DuplicateBaseReflectedFields,
		DuplicateField,
		MalformedSerializer,
		InvalidObjectReference,
		InvalidPath,
		UnsupportedVersion
	};

	struct FArchiveFailure
	{
		EArchiveFailureCode Code = EArchiveFailureCode::InvalidData;
		std::string Path;
		std::string Message;
	};

	class FArchive;
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
		explicit FArchivePathScope(FArchive* InArchive) : Archive(InArchive) {}
		FArchive* Archive = nullptr;
		friend class FArchive;
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
		explicit FArchiveObjectScope(FArchive* InArchive) : Archive(InArchive) {}
		FArchive* Archive = nullptr;
		friend class FArchive;
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
		explicit FArchiveFieldScope(FArchive* InArchive) : Archive(InArchive) {}
		FArchive* Archive = nullptr;
		friend class FArchive;
	};

	class FArchive
	{
	public:
		COREDOBJECT_API explicit FArchive(FArchiveState State, FArchiveVersionContext Versions = {});
		virtual ~FArchive() = default;

		auto IsLoading() const -> bool { return State.Direction == EArchiveDirection::Load; }
		auto IsSaving() const -> bool { return State.Direction == EArchiveDirection::Save; }
		auto IsDiscovering() const -> bool { return State.Purpose == EArchivePurpose::Discovery; }
		auto GetPurpose() const -> EArchivePurpose { return State.Purpose; }
		auto HasCapability(EArchiveCapability Capability) const -> bool
		{
			return (State.Capabilities & Capability) == Capability;
		}
		auto GetVersionContext() const -> const FArchiveVersionContext& { return Versions; }
		auto GetFailure() const -> const FArchiveFailure* { return Failure ? &*Failure : nullptr; }
		auto HasError() const -> bool { return Failure != nullptr; }
		COREDOBJECT_API auto GetError() const -> std::string_view;
		COREDOBJECT_API auto Fail(EArchiveFailureCode Code, std::string_view Message) -> void;
		// Transitional call-site helper; failures are still stored as the structured first failure.
		auto SetError(std::string_view Message) -> void { Fail(EArchiveFailureCode::InvalidData, Message); }

		COREDOBJECT_API auto EnterObject(DObject& Object) -> FArchiveObjectScope;
		COREDOBJECT_API auto EnterField(const FArchiveFieldDescriptor& Field) -> FArchiveFieldScope;
		COREDOBJECT_API auto EnterFixedArrayElement(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto EnterArrayElement(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto EnterMapKey(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto EnterMapValue(uint64 Index) -> FArchivePathScope;
		COREDOBJECT_API auto MarkBaseReflectedFieldsSerialized() -> void;

		virtual COREDOBJECT_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void;
		virtual COREDOBJECT_API auto SerializeObjectReference(DObject*& Value) -> void;
		virtual COREDOBJECT_API auto SerializeSoftObjectPath(FSoftObjectPath& Value) -> void;
		virtual auto GetRemainingPayloadBytes() const -> uint64 { return std::numeric_limits<uint64>::max(); }
		virtual auto IsCurrentFieldAvailable() const -> bool { return true; }

		COREDOBJECT_API auto operator<<(bool& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(int8& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(int16& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(int32& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(int64& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(uint8& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(uint16& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(uint32& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(uint64& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(float& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(double& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(FName& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(FGuid& Value) -> FArchive&;
		COREDOBJECT_API auto operator<<(std::string& Value) -> FArchive&;

		template<typename T> requires std::is_enum_v<T>
		auto operator<<(T& Value) -> FArchive&
		{
			if (!IsCurrentFieldAvailable()) return *this;
			using Underlying = std::underlying_type_t<T>;
			Underlying Encoded = static_cast<Underlying>(Value);
			*this << Encoded;
			if (IsLoading() && !HasError()) Value = static_cast<T>(Encoded);
			return *this;
		}

	protected:
		auto EnableCapabilities(EArchiveCapability Capabilities) -> void { State.Capabilities |= Capabilities; }
		virtual COREDOBJECT_API auto OnEnterObject(DObject& Object) -> void;
		virtual COREDOBJECT_API auto OnLeaveObject() -> void;
		virtual COREDOBJECT_API auto OnEnterField(const FArchiveFieldDescriptor& Field) -> void;
		virtual COREDOBJECT_API auto OnLeaveField() -> void;
		virtual COREDOBJECT_API auto OnEnterFixedArrayElement(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnEnterArrayElement(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnEnterMapKey(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnEnterMapValue(uint64 Index) -> void;
		virtual COREDOBJECT_API auto OnLeavePath() -> void;

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
		COREDOBJECT_API auto FormatFailure() const -> std::string;
		COREDOBJECT_API auto PushPath(std::string Segment) -> void;
		COREDOBJECT_API auto PopPath() -> void;

		FArchiveState State;
		FArchiveVersionContext Versions;
		std::unique_ptr<FArchiveFailure> Failure;
		mutable std::string FormattedFailure;
		std::vector<std::string> PathSegments;
		std::vector<FObjectScopeState> ObjectScopes;
		std::vector<bool> FieldScopes;

		friend class FArchiveObjectScope;
		friend class FArchiveFieldScope;
		friend class FArchivePathScope;
	};

	class FMemoryWriter : public FArchive
	{
	public:
		COREDOBJECT_API explicit FMemoryWriter(
			std::vector<uint8>& InBytes,
			EArchivePurpose Purpose = EArchivePurpose::ObjectGraph);
		COREDOBJECT_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
	private:
		std::vector<uint8>& Bytes;
	};

	class FMemoryReader : public FArchive
	{
	public:
		COREDOBJECT_API explicit FMemoryReader(
			const std::vector<uint8>& InBytes,
			EArchivePurpose Purpose = EArchivePurpose::ObjectGraph);
		COREDOBJECT_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
		auto GetRemainingPayloadBytes() const -> uint64 override
		{
			return static_cast<uint64>(Bytes.size()) - Offset;
		}
	private:
		const std::vector<uint8>& Bytes;
		uint64 Offset = 0;
	};

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
