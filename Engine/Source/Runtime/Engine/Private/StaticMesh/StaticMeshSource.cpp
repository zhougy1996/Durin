#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "StaticMesh/StaticMeshSource.h"
#include "StaticMeshSourceCodec.h"

#include "Materials/MeshMaterialSlot.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		struct FSourceReadCancelled {};
		struct FSourceReadControl
		{
			const std::function<bool()>& ShouldCancel;
			uint32 Work = 0;
			auto Check() const -> void
			{
				if (ShouldCancel && ShouldCancel()) throw FSourceReadCancelled{};
			}
			auto Tick() -> void
			{
				if (++Work < 256) return;
				Work = 0;
				Check();
			}
		};
		auto SerializeImportedString(FArchive& Ar, std::string& Value) -> void
		{
			SerializeBoundedString(Ar, Value, 4096);
		}

		template<typename TValue, typename FSerializeValue>
		auto SerializeImportedArray(FArchive& Ar, std::vector<TValue>& Values,
			uint64 MaximumCount, FSerializeValue&& SerializeValue, FSourceReadControl* Control = nullptr) -> void
		{
			uint64 Count = Values.size();
			Ar << Count;
			if (Ar.IsLoading() && !Ar.IsError())
			{
				constexpr uint64 MinimumWireBytes = [] {
					if constexpr (std::is_same_v<TValue, FMeshPolygonGroup>) return uint64{20};
					else if constexpr (std::is_same_v<TValue, FMeshDescriptionSection>) return uint64{84};
					else if constexpr (std::is_same_v<TValue, FVector2f>) return uint64{8};
					else if constexpr (std::is_same_v<TValue, FVector3f>) return uint64{12};
					else if constexpr (std::is_same_v<TValue, FVector4f>) return uint64{16};
					else return uint64{4};
				}();
				if (Count > MaximumCount || Count > MaximumStaticMeshSourceBytes / sizeof(TValue)
					|| Count > Ar.GetRemainingPayloadBytes() / MinimumWireBytes)
				{
					Ar.Fail(EArchiveFailureCode::LimitExceeded,
						"StaticMesh imported array exceeds its element limit.");
					return;
				}
				if (Control) Control->Check();
				Values.resize(static_cast<size_t>(Count));
			}
			for (TValue& Value : Values)
			{
				if (Control) Control->Tick();
				SerializeValue(Ar, Value);
				if (Ar.IsError()) return;
			}
		}

		auto SerializeStaticMeshSourceGeometry(
			FArchive& Ar, FMeshDescription& Value, FSourceReadControl* Control = nullptr, const uint32* ReadSchema = nullptr) -> void
		{
			// Compact descriptions keep the original canonical encoding and identity.
			uint32 Schema = 1;
			if (Ar.IsSaving() && std::ranges::any_of(Value.Sections,
				[](const auto& Section) { return !Section.VertexInstanceVertices.empty(); })) Schema = StaticMeshSourceGeometryPayloadVersion;
			if (ReadSchema) Schema = *ReadSchema;
			else Ar << Schema;
			if (Ar.IsLoading() && (Schema != 1 && Schema != StaticMeshSourceGeometryPayloadVersion))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh source schema is incompatible.");
				return;
			}
			SerializeImportedArray(Ar, Value.PolygonGroups, MaximumMeshMaterialSlots,
				[](FArchive& Inner, FMeshPolygonGroup& Slot) {
					SerializeImportedString(Inner, Slot.Name);
					Inner << Slot.SourceMaterialIndex;
					SerializeImportedString(Inner, Slot.SourceName);
				}, Control);
			SerializeImportedArray(Ar, Value.Sections, 65536,
				[Control, Schema](FArchive& Inner, FMeshDescriptionSection& Mesh) {
					SerializeImportedString(Inner, Mesh.Name);
					Inner << Mesh.SourceMaterialIndex;
					auto Vector2 = [](FArchive& A, FVector2f& V) { A << V.x << V.y; };
					auto Vector3 = [](FArchive& A, FVector3f& V) { A << V.x << V.y << V.z; };
					auto Vector4 = [](FArchive& A, FVector4f& V) { A << V.x << V.y << V.z << V.w; };
					SerializeImportedArray(Inner, Mesh.Positions, 50'000'000, Vector3, Control);
					SerializeImportedArray(Inner, Mesh.Normals, 50'000'000, Vector3, Control);
					SerializeImportedArray(Inner, Mesh.Tangents, 50'000'000, Vector4, Control);
					for (auto& UVs : Mesh.UVChannels)
						SerializeImportedArray(Inner, UVs, 50'000'000, Vector2, Control);
					SerializeImportedArray(Inner, Mesh.Colors, 50'000'000, Vector4, Control);
					SerializeImportedArray(Inner, Mesh.Indices, 150'000'000,
						[](FArchive& A, uint32& Index) { A << Index; }, Control);
					if (Schema >= 2)
						SerializeImportedArray(Inner, Mesh.VertexInstanceVertices, 50'000'000,
							[](FArchive& A, uint32& Index) { A << Index; }, Control);
				}, Control);
		}

		auto ValidateMeshDescription(
			const FMeshDescription& Value, FStaticMeshSourceError& OutError,
			uint64* OutWireBytes = nullptr, FSourceReadControl* Control = nullptr) -> bool
		{
			if (Value.PolygonGroups.empty() || Value.PolygonGroups.size() > MaximumMeshMaterialSlots
				|| Value.Sections.empty() || Value.Sections.size() > 65536)
			{
				OutError = {.Code = EStaticMeshSourceError::Counts, .SlotCount = Value.PolygonGroups.size(), .MeshCount = Value.Sections.size(), .ExpectedSlotCount = MaximumMeshMaterialSlots, .ExpectedMeshCount = 65536};
				return false;
			}
			uint64 WireBytes = 20;
			const bool bExplicitInstances = std::ranges::any_of(Value.Sections,
				[](const auto& Section) { return !Section.VertexInstanceVertices.empty(); });
			const auto AddBytes = [&](std::string_view Field, uint64 Count, uint64 Width, uint64 MaximumCount) {
				if (Count > MaximumCount || Count > (MaximumStaticMeshSourceBytes - WireBytes) / Width)
				{
					OutError.Code = EStaticMeshSourceError::Limit;
					OutError.Field = Field;
					OutError.Actual = Count;
					OutError.Expected = MaximumCount;
					OutError.Width = Width;
					OutError.WireBytes = WireBytes;
					return false;
				}
				WireBytes += Count * Width;
				return true;
			};
			std::unordered_set<uint32> SourceMaterials;
			for (const FMeshPolygonGroup& Slot : Value.PolygonGroups)
			{
				if (!AddBytes("SlotMetadata", 20, 1, 20) || !AddBytes("SlotName", Slot.Name.size(), 1, 4096)
					|| !AddBytes("SlotSourceName", Slot.SourceName.size(), 1, 4096)) return false;
				if (!SourceMaterials.insert(Slot.SourceMaterialIndex).second)
				{
					OutError.Code = EStaticMeshSourceError::DuplicateMaterial;
					OutError.Actual = Slot.SourceMaterialIndex;
					return false;
				}
			}
			for (const FMeshDescriptionSection& Mesh : Value.Sections)
			{
				OutError.MeshName = Mesh.Name;
				if (bExplicitInstances && (!AddBytes("InstanceCount", 8, 1, 8)
					|| !AddBytes("VertexInstanceVertices", Mesh.VertexInstanceVertices.size(), 4, 50'000'000))) return false;
				for (size_t Instance = 0; Instance < Mesh.VertexInstanceVertices.size(); ++Instance)
				{
					if (Control) Control->Tick();
					if (Mesh.VertexInstanceVertices[Instance] < Mesh.Positions.size()) continue;
					OutError.Code = EStaticMeshSourceError::IndexRange;
					OutError.Field = "VertexInstanceVertices";
					OutError.Index = Instance;
					OutError.Actual = Mesh.VertexInstanceVertices[Instance];
					OutError.Expected = Mesh.Positions.size();
					return false;
				}
				if (!AddBytes("MeshMetadata", 84, 1, 84) || !AddBytes("MeshName", Mesh.Name.size(), 1, 4096)
					|| !AddBytes("Positions", Mesh.Positions.size(), 12, 50'000'000)
					|| !AddBytes("Normals", Mesh.Normals.size(), 12, 50'000'000)
					|| !AddBytes("Tangents", Mesh.Tangents.size(), 16, 50'000'000)
					|| !AddBytes("Colors", Mesh.Colors.size(), 16, 50'000'000)
					|| !AddBytes("Indices", Mesh.Indices.size(), 4, 150'000'000)) return false;
				for (size_t Channel = 0; Channel < Mesh.UVChannels.size(); ++Channel)
				{
					if (AddBytes("UVs", Mesh.UVChannels[Channel].size(), 8, 50'000'000)) continue;
					OutError.Index = Channel;
					return false;
				}
				if (!SourceMaterials.contains(Mesh.SourceMaterialIndex))
				{
					OutError.Code = EStaticMeshSourceError::MissingMaterial;
					OutError.Actual = Mesh.SourceMaterialIndex;
					return false;
				}
				if (Mesh.Positions.empty() || Mesh.Indices.empty() || Mesh.Indices.size() % 3 != 0)
				{
					OutError.Code = Mesh.Positions.empty() || Mesh.Indices.empty() ? EStaticMeshSourceError::EmptyMesh : EStaticMeshSourceError::TriangleList;
					OutError.Field = Mesh.Positions.empty() ? "Positions" : "Indices";
					OutError.Actual = Mesh.Positions.empty() ? 0 : Mesh.Indices.size();
					OutError.Expected = OutError.Code == EStaticMeshSourceError::EmptyMesh ? 1 : 3;
					return false;
				}
				for (size_t Index = 0; Index < Mesh.Positions.size(); ++Index)
				{
					if (Control) Control->Tick();
					if (Math::IsFinite(Mesh.Positions[Index])) continue;
					OutError.Code = EStaticMeshSourceError::NonFinitePosition;
					OutError.Index = Index;
					OutError.Position = Mesh.Positions[Index];
					return false;
				}
				const auto ValidChannel = [&](std::string_view Field, const auto& Channel) {
					if (Channel.empty() || Channel.size() == Mesh.GetVertexInstanceCount()) return true;
					OutError.Code = EStaticMeshSourceError::ChannelLength;
					OutError.Field = Field;
					OutError.Actual = Channel.size();
					OutError.Expected = Mesh.GetVertexInstanceCount();
					return false;
				};
				if (!ValidChannel("Normals", Mesh.Normals) || !ValidChannel("Tangents", Mesh.Tangents)
					|| !ValidChannel("Colors", Mesh.Colors)) return false;
				for (size_t Channel = 0; Channel < Mesh.UVChannels.size(); ++Channel)
				{
					if (ValidChannel("UVs", Mesh.UVChannels[Channel])) continue;
					OutError.Index = Channel;
					return false;
				}
				for (size_t Offset = 0; Offset < Mesh.Indices.size(); ++Offset)
				{
					if (Control) Control->Tick();
					if (Mesh.Indices[Offset] < Mesh.GetVertexInstanceCount()) continue;
					OutError.Code = EStaticMeshSourceError::IndexRange;
					OutError.Index = Offset;
					OutError.Actual = Mesh.Indices[Offset];
					OutError.Expected = Mesh.GetVertexInstanceCount();
					return false;
				}
			}
			if (OutWireBytes) *OutWireBytes = WireBytes;
			OutError = {};
			return true;
		}

	}

	FStaticMeshSource::FStaticMeshSource(const FStaticMeshSource& Other)
	{
		*this = Other;
	}

	auto FStaticMeshSource::operator=(const FStaticMeshSource& Other)
		-> FStaticMeshSource&
	{
		if (this == &Other) return *this;
		std::scoped_lock Lock(ResidencyMutex, Other.ResidencyMutex);
		Geometry = Other.Geometry;
		MaterialSlotCount = Other.MaterialSlotCount;
		MeshCount = Other.MeshCount;
		LODCount = Other.LODCount;
		ResidentGeometry = Other.ResidentGeometry;
		ResidentLODs = Other.ResidentLODs;
		ResidentIdentity = Other.ResidentIdentity;
		return *this;
	}

	auto FormatStaticMeshSourceError(const FStaticMeshSourceError& Error) -> std::string
	{
		std::string_view Reason;
		switch (Error.Code)
		{
		case EStaticMeshSourceError::None: return {};
		case EStaticMeshSourceError::Read: return Error.ReadCause ? FormatPackageResourceReadError(*Error.ReadCause) : "StaticMesh source read failed.";
		case EStaticMeshSourceError::Counts: Reason = "has invalid slot or mesh counts."; break;
		case EStaticMeshSourceError::Limit: Reason = "exceeds its authored count or 1 GiB byte limit."; break;
		case EStaticMeshSourceError::DuplicateMaterial: Reason = "material source indices must be unique."; break;
		case EStaticMeshSourceError::MissingMaterial: Reason = "references a missing source material."; break;
		case EStaticMeshSourceError::EmptyMesh: Reason = "contains an empty mesh."; break;
		case EStaticMeshSourceError::TriangleList: Reason = "index count is not a triangle list."; break;
		case EStaticMeshSourceError::NonFinitePosition: Reason = "contains a non-finite position."; break;
		case EStaticMeshSourceError::ChannelLength: Reason = "vertex channel lengths must match positions."; break;
		case EStaticMeshSourceError::IndexRange: Reason = "contains an out-of-range index."; break;
		case EStaticMeshSourceError::InvalidHeader: Reason = "source header is missing or invalid."; break;
		case EStaticMeshSourceError::PayloadSize: Reason = "payload size does not match metadata."; break;
		case EStaticMeshSourceError::Archive: Reason = "Archive decoding failed."; break;
		case EStaticMeshSourceError::EncodeArchive: Reason = "Archive encoding failed."; break;
		case EStaticMeshSourceError::EncodedSize: Reason = "exceeds the 1 GiB authored limit."; break;
		case EStaticMeshSourceError::BulkUpdate:
			return Error.BulkCause ? FormatEditorBulkDataError(*Error.BulkCause) : "StaticMesh source Bulk update failed.";
		case EStaticMeshSourceError::MetadataCounts: Reason = "source counts do not match metadata."; break;
		case EStaticMeshSourceError::Cancelled: Reason = "read was cancelled."; break;
		}
		return std::format("StaticMesh canonical geometry {}", Reason);
	}

	auto FStaticMeshSource::Initialize(
		FMeshDescription Value) -> std::expected<void, FStaticMeshSourceError>
	{
		uint64 WireBytes = 0;
		FStaticMeshSourceError Validation;
		if (!ValidateMeshDescription(Value, Validation, &WireBytes))
		{
			return std::unexpected(std::move(Validation));
		}
		FByteBuffer Bytes;
		Bytes.reserve(static_cast<size_t>(WireBytes));
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::BulkData);
		SerializeStaticMeshSourceGeometry(Ar, Value);
		if (Ar.IsError())
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::EncodeArchive,
				.ArchiveCode = Ar.GetFailure()->Code, .ArchivePath = Ar.GetFailure()->Path});
		if (Bytes.size() > MaximumStaticMeshSourceBytes)
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::EncodedSize, .Actual = Bytes.size(), .Expected = MaximumStaticMeshSourceBytes});
		FStaticMeshSource Candidate;
		Candidate.Geometry = Geometry;
		if (const auto Updated = Candidate.Geometry.UpdatePayload(FSharedByteBuffer::Take(std::move(Bytes))); !Updated)
		{
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::BulkUpdate, .BulkCause = Updated.error()});
		}
		Candidate.MaterialSlotCount = static_cast<uint32>(Value.PolygonGroups.size());
		Candidate.MeshCount = static_cast<uint32>(Value.Sections.size());
		Candidate.ResidentGeometry = std::make_shared<const FMeshDescription>(std::move(Value));
		Candidate.ResidentLODs = {Candidate.ResidentGeometry};
		Candidate.ResidentIdentity = Candidate.GetIdentity();
		*this = Candidate;
		return {};
	}

	auto FStaticMeshSource::InitializeLODs(std::vector<FMeshDescription> Values)
		-> std::expected<void, FStaticMeshSourceError>
	{
		if (Values.empty() || Values.size() > MaximumStaticMeshSourceLODs)
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Counts});
		if (Values.size() == 1) return Initialize(std::move(Values.front()));
		for (const auto& Value : Values)
			if (Value.PolygonGroups.empty() || Value.PolygonGroups.size() > MaximumMeshMaterialSlots
				|| Value.Sections.empty() || Value.Sections.size() > 65536)
				return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Counts});
		// All LODs share one source-material namespace. Include materials used only
		// by lower LODs in the primary descriptor for stable slot reconciliation.
		for (size_t LOD = 1; LOD < Values.size(); ++LOD)
			for (const auto& Group : Values[LOD].PolygonGroups)
				if (std::ranges::find(Values.front().PolygonGroups, Group.SourceMaterialIndex,
					&FMeshPolygonGroup::SourceMaterialIndex) == Values.front().PolygonGroups.end())
				{
					if (Values.front().PolygonGroups.size() == MaximumMeshMaterialSlots)
						return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Counts});
					Values.front().PolygonGroups.push_back(Group);
				}
		uint64 TotalBytes = 8;
		uint32 Sections = 0;
		for (const auto& Value : Values)
		{
			uint64 Bytes = 0;
			FStaticMeshSourceError Error;
			if (!ValidateMeshDescription(Value, Error, &Bytes)) return std::unexpected(std::move(Error));
			if (Bytes > MaximumStaticMeshSourceBytes - TotalBytes)
				return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::EncodedSize});
			TotalBytes += Bytes;
			Sections += static_cast<uint32>(Value.Sections.size());
		}
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::BulkData);
		uint32 Schema = StaticMeshSourceLODsPayloadVersion, Count = static_cast<uint32>(Values.size());
		Ar << Schema << Count;
		for (auto& Value : Values) SerializeStaticMeshSourceGeometry(Ar, Value);
		if (Ar.IsError() || Bytes.size() > MaximumStaticMeshSourceBytes)
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::EncodeArchive});
		FStaticMeshSource Candidate;
		Candidate.Geometry = Geometry;
		if (const auto Updated = Candidate.Geometry.UpdatePayload(FSharedByteBuffer::Take(std::move(Bytes))); !Updated)
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::BulkUpdate, .BulkCause = Updated.error()});
		Candidate.MaterialSlotCount = static_cast<uint32>(Values.front().PolygonGroups.size());
		Candidate.MeshCount = Sections;
		Candidate.LODCount = Count;
		for (auto& Value : Values) Candidate.ResidentLODs.push_back(std::make_shared<const FMeshDescription>(std::move(Value)));
		Candidate.ResidentGeometry = Candidate.ResidentLODs.front();
		Candidate.ResidentIdentity = Candidate.GetIdentity();
		*this = Candidate;
		return {};
	}

	auto StaticMeshPrivate::DecodeSourceLODs(FByteView Bytes, uint32 MaterialSlotCount, uint32 MeshCount,
		const std::function<bool()>& ShouldCancel) -> std::expected<std::vector<FMeshDescriptionReadHandle>, FStaticMeshSourceError>
	try
	{
		FSourceReadControl Control{ShouldCancel};
		Control.Check();
		if (Bytes.empty() || Bytes.size() > MaximumStaticMeshSourceBytes || !MaterialSlotCount
			|| MaterialSlotCount > MaximumMeshMaterialSlots || !MeshCount || MeshCount > 65536 * MaximumStaticMeshSourceLODs)
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::InvalidHeader});
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::BulkData);
		uint32 Schema = 0, Count = 1, Sections = 0;
		Ar << Schema;
		if (Schema == StaticMeshSourceLODsPayloadVersion) Ar << Count;
		if (Ar.IsError() || Count == 0 || Count > MaximumStaticMeshSourceLODs)
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::InvalidHeader});
		std::vector<FMeshDescriptionReadHandle> Result;
		for (uint32 LOD = 0; LOD < Count; ++LOD)
		{
			auto Decoded = std::make_shared<FMeshDescription>();
			SerializeStaticMeshSourceGeometry(Ar, *Decoded, &Control, Schema == StaticMeshSourceLODsPayloadVersion ? nullptr : &Schema);
			if (Ar.IsError()) break;
			FStaticMeshSourceError Validation;
			if (!ValidateMeshDescription(*Decoded, Validation, nullptr, &Control)) return std::unexpected(std::move(Validation));
			Sections += static_cast<uint32>(Decoded->Sections.size());
			Result.push_back(std::move(Decoded));
		}
		if (Ar.IsError() || !RequireArchiveEnd(Ar))
		{
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Archive, .Actual = Ar.Tell(), .Expected = Bytes.size(),
				.ArchiveCode = Ar.GetFailure()->Code, .ArchivePath = Ar.GetFailure()->Path});
		}
		if (Result.front()->PolygonGroups.size() != MaterialSlotCount || Sections != MeshCount)
		{
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::MetadataCounts, .SlotCount = Result.front()->PolygonGroups.size(), .MeshCount = Sections,
				.ExpectedSlotCount = MaterialSlotCount, .ExpectedMeshCount = MeshCount});
		}
		std::unordered_set<uint32> PrimaryMaterials;
		for (const auto& Group : Result.front()->PolygonGroups) PrimaryMaterials.insert(Group.SourceMaterialIndex);
		for (const auto& LOD : Result)
			for (const auto& Group : LOD->PolygonGroups)
				if (!PrimaryMaterials.contains(Group.SourceMaterialIndex))
					return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::MissingMaterial});
		Control.Check();
		return Result;
	}
	catch (const FSourceReadCancelled&)
	{
		return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Cancelled});
	}

	auto StaticMeshPrivate::DecodeSourceGeometry(FByteView Bytes, uint32 MaterialSlotCount, uint32 MeshCount,
		const std::function<bool()>& ShouldCancel) -> std::expected<FMeshDescriptionReadHandle, FStaticMeshSourceError>
	{
		auto LODs = DecodeSourceLODs(Bytes, MaterialSlotCount, MeshCount, ShouldCancel);
		if (!LODs) return std::unexpected(LODs.error());
		return LODs->front();
	}
	auto FStaticMeshSource::AcquireGeometry(const std::function<bool()>& ShouldCancel) const
		-> std::expected<FMeshDescriptionReadHandle, FStaticMeshSourceError>
	{
		return AcquireLODGeometry(0, ShouldCancel);
	}

	auto FStaticMeshSource::AcquireLODGeometry(uint32 LODIndex, const std::function<bool()>& ShouldCancel) const
		-> std::expected<FMeshDescriptionReadHandle, FStaticMeshSourceError>
	{
		auto LODs = AcquireLODs(ShouldCancel);
		if (!LODs) return std::unexpected(LODs.error());
		if (LODIndex >= LODs->size()) return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Counts});
		return (*LODs)[LODIndex];
	}

	auto FStaticMeshSource::AcquireLODs(const std::function<bool()>& ShouldCancel) const
		-> std::expected<std::vector<FMeshDescriptionReadHandle>, FStaticMeshSourceError>
	{
		std::lock_guard Lock(ResidencyMutex);
		FSourceReadControl Control{ShouldCancel};
		try
		{
			Control.Check();
			const FXxHash128 Identity = GetIdentity();
			if (ResidentGeometry && ResidentIdentity == Identity && ResidentLODs.size() == LODCount) return ResidentLODs;
			ResidentGeometry.reset();
			ResidentLODs.clear();
			if (!IsValid())
			{
				return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::InvalidHeader, .SlotCount = MaterialSlotCount, .MeshCount = MeshCount});
			}
			const FPackageResourceReadResult Payload = Geometry.GetPayload().Wait();
			Control.Check();
			if (!Payload)
			{
				return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Read, .ReadCause = Payload});
			}
			const FByteView Bytes = Payload->GetBytes();
			if (Bytes.size() != Geometry.GetPayloadSize() || Bytes.size() > MaximumStaticMeshSourceBytes)
			{
				return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::PayloadSize, .Actual = Bytes.size(), .Expected = Geometry.GetPayloadSize()});
			}
			auto Decoded = StaticMeshPrivate::DecodeSourceLODs(Bytes, MaterialSlotCount, MeshCount, ShouldCancel);
			if (!Decoded) return std::unexpected(std::move(Decoded.error()));
			if (Decoded->size() != LODCount)
				return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::MetadataCounts});
			ResidentIdentity = Identity;
			ResidentLODs = std::move(*Decoded);
			ResidentGeometry = ResidentLODs.front();
			return ResidentLODs;
		}
		catch (const FSourceReadCancelled&)
		{
			return std::unexpected(FStaticMeshSourceError{.Code = EStaticMeshSourceError::Cancelled});
		}
	}

	auto FStaticMeshSource::ReleaseGeometry() const -> void
	{
		std::lock_guard Lock(ResidencyMutex);
		ResidentGeometry.reset();
		ResidentLODs.clear();
	}

	auto FStaticMeshSource::IsGeometryResident() const -> bool
	{
		std::lock_guard Lock(ResidencyMutex);
		return ResidentGeometry && ResidentIdentity == GetIdentity();
	}

	auto FStaticMeshSource::IsValid() const -> bool
	{
		return MaterialSlotCount > 0 && MaterialSlotCount <= MaximumMeshMaterialSlots
			&& LODCount > 0 && LODCount <= MaximumStaticMeshSourceLODs
			&& MeshCount > 0 && MeshCount <= 65536 * LODCount
			&& Geometry.GetPayloadSize() > 0
			&& Geometry.GetPayloadSize() <= MaximumStaticMeshSourceBytes;
	}

	auto FStaticMeshSource::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		return StaticMeshPrivate::BuildSourceIdentity(MaterialSlotCount, MeshCount, Geometry.GetPayloadId(), LODCount);
	}

}

#endif
