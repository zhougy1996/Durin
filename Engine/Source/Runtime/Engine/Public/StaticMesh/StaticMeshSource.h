#pragma once

#include "Asset/EditorBulkData.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "StaticMeshSource.gen.h"

namespace Durin
{
	inline constexpr FGuid StaticMeshSourceGeometryPayloadId{
		0x442898cd, 0x801d49ed, 0x93459533, 0x4531fc1d};
	inline constexpr uint32 StaticMeshSourceSchemaVersion = 1;
	inline constexpr uint64 MaximumStaticMeshSourceBytes =
		1024ull * 1024ull * 1024ull;

	// Canonical authored value. Mutation/reflection loading requires exclusive owner access.
	// Stable values support concurrent acquire, release and copy; handles are always immutable.
	DSTRUCT(LegacyNames = "Durin::FStaticMeshImportedData")
	struct FStaticMeshSource
	{
		GENERATED_BODY()

	public:
		FStaticMeshSource() = default;
		ENGINE_API FStaticMeshSource(const FStaticMeshSource& Other);
		ENGINE_API auto operator=(const FStaticMeshSource& Other) -> FStaticMeshSource&;

		// Validates the complete candidate before replacement and seeds residency without decoding.
		ENGINE_API auto Initialize(FStaticMeshDecodedGeometry Value, std::string& OutError) -> bool;
		// May read bulk and block. Concurrent callers share one successful decode; failures are not cached.
		// Cancellation is borrowed under the residency lock and must not reenter this source.
		ENGINE_API auto AcquireGeometry(std::string& OutError,
			const std::function<bool()>& ShouldCancel = {}) const -> FStaticMeshGeometryReadHandle;
		// Drops only this value's decoded ownership, never canonical bulk or outstanding readers.
		ENGINE_API auto ReleaseGeometry() const -> void;
		ENGINE_API auto IsGeometryResident() const -> bool;
		// Metadata queries never acquire geometry or read payload bytes.
		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
		auto GetGeometryBulk() const -> const FEditorBulkData& { return Geometry; }
		auto GetMaterialSlotCount() const -> uint32 { return MaterialSlotCount; }
		auto GetMeshCount() const -> uint32 { return MeshCount; }
		auto GetSchemaVersion() const -> uint32 { return SchemaVersion; }

	private:
		DPROPERTY()
		FEditorBulkData Geometry;

		DPROPERTY()
		uint32 MaterialSlotCount = 0;

		DPROPERTY()
		uint32 MeshCount = 0;

		DPROPERTY()
		uint32 SchemaVersion = StaticMeshSourceSchemaVersion;

		mutable std::mutex ResidencyMutex;
		mutable FStaticMeshGeometryReadHandle ResidentGeometry;
		// Reflection can replace persistent fields without invoking assignment.
		mutable FXxHash128 ResidentIdentity;
	};
}
