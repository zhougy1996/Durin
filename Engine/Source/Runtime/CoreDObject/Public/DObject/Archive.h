#pragma once

#include "CoreDObjectAPI.h"
#include "DObjectFwd.h"

namespace Durin
{
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

		virtual auto SerializeBytes(void* Data, uint64 Size) -> void = 0;
		virtual auto SerializeObjectReference(DObject*& Object) -> void = 0;

		template<typename T>
		auto operator<<(T& Value) -> FArchive&
		{
			SerializeBytes(&Value, sizeof(T));
			return *this;
		}

		COREDOBJECT_API auto SerializeString(std::string& Value) -> void;

	private:
		EMode Mode;
	};

	class FMemoryWriter : public FArchive
	{
	public:
		COREDOBJECT_API explicit FMemoryWriter(std::vector<uint8>& InBytes);
		COREDOBJECT_API auto SerializeBytes(void* Data, uint64 Size) -> void override;
		COREDOBJECT_API auto SerializeObjectReference(DObject*& Object) -> void override;

	private:
		std::vector<uint8>& Bytes;
	};

	class FMemoryReader : public FArchive
	{
	public:
		COREDOBJECT_API explicit FMemoryReader(const std::vector<uint8>& InBytes);
		COREDOBJECT_API auto SerializeBytes(void* Data, uint64 Size) -> void override;
		COREDOBJECT_API auto SerializeObjectReference(DObject*& Object) -> void override;

	private:
		const std::vector<uint8>& Bytes;
		uint64 Offset = 0;
	};

	COREDOBJECT_API auto SerializeDObjectProperties(FArchive& Ar, DObject* Object) -> void;
	COREDOBJECT_API auto SaveObjectGraphToMemory(DObject* RootObject, std::vector<uint8>& OutBytes) -> bool;
	COREDOBJECT_API auto LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes) -> DObject*;
}
