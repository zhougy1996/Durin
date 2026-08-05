#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectFwd.h"
#include "Misc/Name.h"

#include <limits>
#include <string>
#include <string_view>

namespace Durin
{
	class FProperty;
	class FPropertyValueSnapshot;

	COREDOBJECT_API auto CapturePropertyValue(
		const FProperty* Property,
		const void* Container,
		uint32 ArrayIndex,
		FPropertyValueSnapshot& OutSnapshot,
		std::string* OutError = nullptr
	) -> bool;
	COREDOBJECT_API auto RestorePropertyValue(
		const FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		const FPropertyValueSnapshot& Snapshot,
		std::string* OutError = nullptr
	) -> bool;

	// Defines directional byte and object-reference serialization for reflected object graphs.
	class FArchive
	{
	public:
		enum class EMode
		{
			Load,
			Save
		};

		explicit FArchive(EMode InMode)
			: Mode(InMode)
		{
		}
		virtual ~FArchive() = default;

		auto IsLoading() const -> bool { return Mode == EMode::Load; }
		auto IsSaving() const -> bool { return Mode == EMode::Save; }
		auto HasError() const -> bool { return !Error.empty(); }
		auto GetError() const -> std::string_view { return Error; }

		virtual auto SerializeBytes(void* Data, uint64 Size) -> void = 0;
		virtual auto SerializeObjectReference(DObject*& Object) -> void = 0;
		virtual auto GetRemainingBytes() const -> uint64
		{
			return std::numeric_limits<uint64>::max();
		}

		template<typename T>
		auto operator<<(T& Value) -> FArchive&
		{
			SerializeBytes(&Value, sizeof(T));
			return *this;
		}

		COREDOBJECT_API auto SerializeString(std::string& Value) -> void;
		COREDOBJECT_API auto SetError(std::string_view Message) -> void;

	private:
		EMode Mode;
		std::string Error;
	};

	// Appends serialized graph data to a caller-owned byte buffer.
	class FMemoryWriter : public FArchive
	{
	public:
		COREDOBJECT_API explicit FMemoryWriter(std::vector<uint8>& InBytes);
		COREDOBJECT_API auto SerializeBytes(void* Data, uint64 Size) -> void override;
		COREDOBJECT_API auto SerializeObjectReference(DObject*& Object) -> void override;

	private:
		std::vector<uint8>& Bytes;
	};

	// Reads serialized graph data from a caller-owned byte buffer.
	class FMemoryReader : public FArchive
	{
	public:
		COREDOBJECT_API explicit FMemoryReader(const std::vector<uint8>& InBytes);
		COREDOBJECT_API auto SerializeBytes(void* Data, uint64 Size) -> void override;
		COREDOBJECT_API auto SerializeObjectReference(DObject*& Object) -> void override;
		auto GetRemainingBytes() const -> uint64 override
		{
			return static_cast<uint64>(Bytes.size()) - Offset;
		}

	private:
		const std::vector<uint8>& Bytes;
		uint64 Offset = 0;
	};

	// Owns detached reflected property storage and roots referenced objects for safe restoration.
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
		// Snapshot references are rooted independently of the edited object's current
		// value so cancel and future transactions cannot restore a dangling object.
		std::vector<DObject*> ReferencedObjects;

		auto AddReferenceRoots() -> void;
		auto ReleaseReferenceRoots() -> void;

		friend COREDOBJECT_API auto CapturePropertyValue(
			const FProperty*, const void*, uint32, FPropertyValueSnapshot&, std::string*
		) -> bool;
		friend COREDOBJECT_API auto RestorePropertyValue(
			const FProperty*, void*, uint32, const FPropertyValueSnapshot&, std::string*
		) -> bool;
	};

	COREDOBJECT_API auto SerializeReflectedPropertyValue(
		FArchive& Ar,
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex = 0,
		bool bIncludeRawObjectReferences = false
	) -> void;
	COREDOBJECT_API auto SerializeDObjectProperties(FArchive& Ar, DObject* Object) -> void;
	COREDOBJECT_API auto SaveObjectGraphToMemory(DObject* RootObject, std::vector<uint8>& OutBytes) -> bool;
	COREDOBJECT_API auto LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes) -> DObject*;
	// Duplicates only the RootObject outer tree. References outside that tree remain shared,

		// which is required for transient runtime copies that still reference persistent assets.
	COREDOBJECT_API auto DuplicateObjectGraph(
		DObject* RootObject,
		DObject* NewOuter,
		FName NewName = FName(),
		std::string* OutError = nullptr,
		std::unordered_map<DObject*, DObject*>* OutDuplicates = nullptr
	) -> DObject*;
	// Copies only reflected Edit properties and remaps object references through the supplied map.
	// This keeps runtime-to-editor apply from replacing structural ownership arrays.
	COREDOBJECT_API auto CopyEditableObjectProperties(
		DObject* Source,
		DObject* Destination,
		const std::unordered_map<DObject*, DObject*>& ReferenceMap,
		std::string* OutError = nullptr
	) -> bool;
}
