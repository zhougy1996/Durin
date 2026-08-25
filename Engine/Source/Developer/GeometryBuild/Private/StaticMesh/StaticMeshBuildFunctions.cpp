#include "StaticMesh/StaticMeshBuildFunctions.h"

#include "Serialization/BinaryFormat.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshBuildDerivedData.h"
#include "StaticMesh/StaticMeshDerivedData.h"

namespace Durin::Asset::Build::Private
{
	const FBuildFunctionIdentity StaticMeshFunctionIdentity{
		"Durin.GeometryBuild.StaticMesh", 1};
	const FBuildFunctionIdentity StaticMeshCollisionFunctionIdentity{
		"Durin.GeometryBuild.StaticMeshCollision", 1};

	namespace
	{
		constexpr uint64 StaticMeshDerivedDataBudgetBytes =
			8ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 StaticMeshDerivedDataCleanupDeleteLimit = 16;

		auto DecodeStaticMeshCollisionInput(std::span<const std::byte> Bytes,
			std::vector<FVector3>& Positions, std::vector<uint32>& Indices,
			EBodySetupCollisionSourceMode& Mode,
			EBodySetupCollisionQueryPolicy& Policy,
			std::string& OutError) -> bool
		{
			FBinaryReader Reader(Bytes);
			uint32 ModeValue = 0, PolicyValue = 0;
			uint64 Count = 0;
			if (!Reader.ReadU32(ModeValue)
				|| !Reader.ReadU32(PolicyValue)
				|| !Reader.ReadU64(Count)
				|| Count > Reader.GetRemainingBytes() / 12)
				goto Invalid;
			Mode = static_cast<EBodySetupCollisionSourceMode>(ModeValue);
			Policy = static_cast<EBodySetupCollisionQueryPolicy>(PolicyValue);
			Positions.reserve(Count);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint32 X = 0, Y = 0, Z = 0;
				if (!Reader.ReadU32(X)
					|| !Reader.ReadU32(Y)
					|| !Reader.ReadU32(Z)) goto Invalid;
				Positions.emplace_back(
					std::bit_cast<float>(X), std::bit_cast<float>(Y), std::bit_cast<float>(Z));
			}
			if (!Reader.ReadU64(Count)
				|| Count > Reader.GetRemainingBytes() / 4)
				goto Invalid;
			Indices.reserve(Count);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				uint32 Value = 0;
				if (!Reader.ReadU32(Value)) goto Invalid;
				Indices.push_back(Value);
			}
			if (Reader.IsAtEnd() && !Positions.empty() && !Indices.empty()
				&& Indices.size() % 3 == 0) return true;
		Invalid:
			OutError = "StaticMesh collision local build input is malformed.";
			return false;
		}

		class FStaticMeshBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheBucket = "StaticMesh/Objects",
					.ExpectedValueName = std::string(StaticMeshValueName),
					.MaximumValueBytes = MaximumStaticMeshPayloadBytes,
					.CleanupBudgetBytes = StaticMeshDerivedDataBudgetBytes,
					.CleanupDeleteLimit = StaticMeshDerivedDataCleanupDeleteLimit};
			}

			auto Validate(const FBuildDefinition&, const FBuildValue& Value,
				std::string& Error) const -> bool override
			{
				std::unique_ptr<FStaticMeshRenderData> Data;
				return DecodeStaticMeshRenderData(Value, Data, Error);
			}

			auto Build(const FBuildContext& Context, FBuildValue& Value,
				std::string& Error) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(StaticMeshInputName);
				if (!Input)
				{
					Error = "StaticMesh build input is missing.";
					return false;
				}
				Value = FBuildValue::FromOwned(std::string(StaticMeshValueName),
					std::vector<std::byte>(Input->GetBytes().begin(), Input->GetBytes().end()));
				return true;
			}
		};

		class FStaticMeshCollisionBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheBucket = "StaticMeshCollision/Objects",
					.ExpectedValueName = std::string(CollisionValueName),
					.MaximumValueBytes = MaximumStaticMeshCollisionPayloadBytes};
			}

			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value,
				std::string& Error) const -> bool override
			{
				const auto ModeFact = Definition.GetTargetFact("Mode");
				const auto PolicyFact = Definition.GetTargetFact("Policy");
				uint32 ModeValue = 0, PolicyValue = 0;
				if (!ModeFact || !PolicyFact
					|| !ParseBuildTargetFactUInt32(*ModeFact, ModeValue)
					|| !ParseBuildTargetFactUInt32(*PolicyFact, PolicyValue))
				{
					Error = "StaticMesh collision target facts are missing.";
					return false;
				}
				FCollisionGeometryRef Geometry;
				return DecodeStaticMeshCollisionValue(Value,
					static_cast<EBodySetupCollisionSourceMode>(ModeValue),
					static_cast<EBodySetupCollisionQueryPolicy>(PolicyValue),
					Geometry, Error);
			}

			auto Build(const FBuildContext& Context, FBuildValue& Value,
				std::string& Error) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(CollisionInputName);
				std::vector<FVector3> Positions;
				std::vector<uint32> Indices;
				EBodySetupCollisionSourceMode Mode;
				EBodySetupCollisionQueryPolicy Policy;
				if (!Input || !DecodeStaticMeshCollisionInput(
					Input->GetBytes(), Positions, Indices, Mode, Policy, Error)) return false;
				FCollisionGeometryBuildDiagnostics Facts;
				FCollisionGeometryRef Geometry =
					Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
					? FCollisionGeometryRef::BuildConvexHull(Positions, &Facts)
					: FCollisionGeometryRef::BuildTriangleMesh(Positions, Indices, &Facts);
				if (!Geometry)
				{
					Error = std::format("StaticMesh collision build failed with status {}.",
						static_cast<uint32>(Facts.Status));
					return false;
				}
				FStaticMeshCollisionPayloadData Payload;
				if (!MakeStaticMeshCollisionPayloadData(Geometry, Policy, Payload, Error))
					return false;
				std::vector<std::byte> Bytes;
				FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
				Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
				if (Ar.HasError())
				{
					Error = Ar.GetError();
					return false;
				}
				Value = FBuildValue::FromOwned(std::string(CollisionValueName), std::move(Bytes));
				return true;
			}
		};
	}

	auto BuildCollisionGeometryHash(
		std::span<const FVector3f> Positions,
		std::span<const uint32> Indices) -> FXxHash128
	{
		FBinaryWriter Writer;
		Writer.WriteU64(Positions.size());
		for (const FVector3f& Position : Positions)
			for (uint32 Axis = 0; Axis < 3; ++Axis)
				Writer.WriteU32(std::bit_cast<uint32>(Position[Axis]));
		Writer.WriteU64(Indices.size());
		for (uint32 Index : Indices) Writer.WriteU32(Index);
		return FXxHash128::HashBuffer(Writer.GetBytes());
	}

	auto EncodeStaticMeshRenderData(
		const FStaticMeshRenderData& RenderData, FBuildValue& OutValue,
		std::string& OutError) -> bool
	{
		FStaticMeshPayloadData Payload;
		if (!MakeStaticMeshPayloadData(RenderData, Payload, OutError)) return false;
		std::vector<std::byte> Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
		if (Ar.HasError())
		{
			OutError = Ar.GetFailure()->Message;
			return false;
		}
		OutValue = FBuildValue::FromOwned(std::string(StaticMeshValueName), std::move(Bytes));
		return true;
	}

	auto DecodeStaticMeshRenderData(
		const FBuildValue& Value,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::string& OutError) -> bool
	{
		FStaticMeshPayloadData Payload;
		FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
		Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
		if (Ar.HasError() || !RequireArchiveEnd(Ar))
		{
			OutError = Ar.GetFailure()
				? Ar.GetFailure()->Message : "StaticMesh payload has trailing bytes.";
			return false;
		}
		return MakeStaticMeshRenderData(Payload, OutRenderData, OutError);
	}

	auto EncodeStaticMeshCollisionInput(
		std::span<const FVector3f> Positions,
		std::span<const uint32> Indices,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy) -> std::vector<std::byte>
	{
		FBinaryWriter Writer;
		Writer.WriteU32(static_cast<uint32>(Mode));
		Writer.WriteU32(static_cast<uint32>(Policy));
		Writer.WriteU64(Positions.size());
		for (const FVector3f& Position : Positions)
			for (uint32 Axis = 0; Axis < 3; ++Axis)
				Writer.WriteU32(std::bit_cast<uint32>(Position[Axis]));
		Writer.WriteU64(Indices.size());
		for (uint32 Index : Indices) Writer.WriteU32(Index);
		return Writer.TakeBytes();
	}

	auto DecodeStaticMeshCollisionValue(
		const FBuildValue& Value,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FCollisionGeometryRef& OutGeometry,
		std::string& OutError) -> bool
	{
		FStaticMeshCollisionPayloadData Payload;
		FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
		Payload.Serialize(Ar, EStaticMeshTargetPlatform::Win64);
		if (Ar.HasError() || !RequireArchiveEnd(Ar) || Payload.SourceMode != Mode)
		{
			OutError = Ar.GetError().empty()
				? "StaticMesh collision payload is incompatible." : Ar.GetError();
			return false;
		}
		Payload.QueryPolicy = Policy;
		return MakeStaticMeshCollisionGeometry(Payload, OutGeometry, OutError);
	}

	auto CreateStaticMeshBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FStaticMeshBuildFunction>();
	}

	auto CreateStaticMeshCollisionBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FStaticMeshCollisionBuildFunction>();
	}
}
