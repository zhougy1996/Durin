#pragma once

#include "Asset/EditorBulkData.h"
#include "StaticMesh/StaticMeshGeometry.h"
#include "StaticMeshSource.gen.h"

namespace Durin
{
	inline constexpr FGuid StaticMeshSourceGeometryPayloadId{
		0x442898cd, 0x801d49ed, 0x93459533, 0x4531fc1d};
	inline constexpr uint32 StaticMeshSourceGeometryPayloadVersion = 1;
	inline constexpr uint64 MaximumStaticMeshSourceBytes =
		1024ull * 1024ull * 1024ull;

	enum class EArchiveFailureCode : uint8;
	enum class EStaticMeshSourceError : uint8
	{
		None, Counts, Limit, DuplicateMaterial, MissingMaterial, EmptyMesh, TriangleList,
		NonFinitePosition, ChannelLength, IndexRange, InvalidHeader, Read, PayloadSize,
		Archive, MetadataCounts, Cancelled, EncodeArchive, EncodedSize, BulkUpdate
	};
	struct FStaticMeshSourceError
	{
		EStaticMeshSourceError Code = EStaticMeshSourceError::None;
		std::string MeshName;
		std::string Field;
		uint64 Index = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 Width = 0;
		uint64 WireBytes = 0;
		uint64 SlotCount = 0;
		uint64 MeshCount = 0;
		uint64 ExpectedSlotCount = 0;
		uint64 ExpectedMeshCount = 0;
		FVector3f Position = FVector3f(0);
		std::optional<EArchiveFailureCode> ArchiveCode;
		std::string ArchivePath;
		std::optional<FPackageResourceReadResult> ReadCause;
		std::optional<FEditorBulkDataError> BulkCause;
	};
	struct FStaticMeshSourceResult
	{
		FStaticMeshSourceError Error;
		explicit operator bool() const { return Error.Code == EStaticMeshSourceError::None; }
	};
	struct FStaticMeshSourceReadResult
	{
		FStaticMeshGeometryReadHandle Geometry;
		FStaticMeshSourceError Error;
		explicit operator bool() const { return Error.Code == EStaticMeshSourceError::None; }
	};
	ENGINE_API auto FormatStaticMeshSourceError(const FStaticMeshSourceError& Error) -> std::string;

	// Canonical authored value. Mutation/reflection loading requires exclusive owner access.
	// Stable values support concurrent acquire, release and copy; handles are always immutable.
	DSTRUCT()
	struct FStaticMeshSource
	{
		GENERATED_BODY()

	public:
		FStaticMeshSource() = default;
		ENGINE_API FStaticMeshSource(const FStaticMeshSource& Other);
		ENGINE_API auto operator=(const FStaticMeshSource& Other) -> FStaticMeshSource&;

		// Validates the complete candidate before replacement and seeds residency without decoding.
		ENGINE_API auto Initialize(FStaticMeshDecodedGeometry Value) -> FStaticMeshSourceResult;
		// May read bulk and block. Concurrent callers share one successful decode; failures are not cached.
		// Cancellation is borrowed under the residency lock and must not reenter this source.
		ENGINE_API auto AcquireGeometry(
			const std::function<bool()>& ShouldCancel = {}) const -> FStaticMeshSourceReadResult;
		// Drops only this value's decoded ownership, never canonical bulk or outstanding readers.
		ENGINE_API auto ReleaseGeometry() const -> void;
		ENGINE_API auto IsGeometryResident() const -> bool;
		// Metadata queries never acquire geometry or read payload bytes.
		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
		auto GetGeometryBulk() const -> const FEditorBulkData& { return Geometry; }
		auto GetMaterialSlotCount() const -> uint32 { return MaterialSlotCount; }
		auto GetMeshCount() const -> uint32 { return MeshCount; }

	private:
		DPROPERTY()
		FEditorBulkData Geometry;

		DPROPERTY()
		uint32 MaterialSlotCount = 0;

		DPROPERTY()
		uint32 MeshCount = 0;

		mutable std::mutex ResidencyMutex;
		mutable FStaticMeshGeometryReadHandle ResidentGeometry;
		// Reflection can replace persistent fields without invoking assignment.
		mutable FXxHash128 ResidentIdentity;
	};
}
