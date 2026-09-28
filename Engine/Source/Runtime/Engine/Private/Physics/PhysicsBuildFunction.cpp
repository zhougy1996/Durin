#include "PhysicsBuildFunction.h"
#if DURIN_WITH_EDITOR
#include "PhysicsCookDerivedDataKey.h"
#include "PhysicsCookInputPrivate.h"
#include "PhysicsSharedOutput.h"
#include <cstring>

namespace Durin::PhysicsPrivate
{
	using namespace DerivedData;
	namespace
	{
		auto Error(std::string Message, EBuildFailureReason Category = EBuildFailureReason::InvalidInput) -> FBuildFailure
		{ return {.Reason = Category, .Description = std::move(Message)}; }
		auto Cancelled() -> FBuildFailure { return Error("Physics cooking was cancelled.", EBuildFailureReason::InternalFailure); }
		auto Reference(FXxHash128 Identity) -> FBuildInputReference
		{ return {"Geometry", Identity, "CollisionGeometry", 1, "TriangleMesh.PositionsIndices", 1}; }
		struct FSettings { EBodySetupCollisionSourceMode Mode; EBodySetupCollisionQueryPolicy Policy; };
		auto Settings(const FBuildAction& Action) -> std::expected<FSettings, FBuildFailure>
		{
			const uint64 *Mode = nullptr, *Policy = nullptr, *Target = nullptr, *Weld = nullptr;
			for (const auto& Constant : Action.GetConstants())
			{
				if (Constant.Name == "SourceMode") Mode = std::get_if<uint64>(&Constant.Value);
				if (Constant.Name == "QueryPolicy") Policy = std::get_if<uint64>(&Constant.Value);
				if (Constant.Name == "TargetPlatform") Target = std::get_if<uint64>(&Constant.Value);
				if (Constant.Name == "WeldToleranceBits") Weld = std::get_if<uint64>(&Constant.Value);
			}
			if (Action.GetConstants().size() != 4 || !Mode || !Policy || !Target || !Weld || *Weld
				|| *Target != uint64(EAssetPayloadTargetPlatform::Win64) || *Mode < 1 || *Mode > 2 || *Policy > 2)
				return std::unexpected(Error("Physics action constants are invalid."));
			return FSettings{EBodySetupCollisionSourceMode(*Mode), EBodySetupCollisionQueryPolicy(*Policy)};
		}
		class FResolver final : public IBuildInputResolver
		{
		public:
			explicit FResolver(FPhysicsCookInput Input) : Input(std::move(Input)) {}
			auto Describe(std::span<const FBuildSourceReference> Sources, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> override
			{
				if (Cancel.IsCancelled()) return std::unexpected(Cancelled());
				if (Sources.size() != 1 || Sources[0].Name != "Geometry" || Sources[0].Source != "CapturedGeometry" || Input.GetIdentity().IsZero())
					return std::unexpected(Error("Physics captured source binding is invalid."));
				return std::vector{Reference(Input.GetIdentity())};
			}
			auto Resolve(std::span<const FBuildInputReference> Inputs, const FBuildCancellation& Cancel) const
				-> std::expected<std::vector<FBuildInput>, FBuildFailure> override
			{
				if (Cancel.IsCancelled()) return std::unexpected(Cancelled());
				if (Inputs.size() != 1 || Inputs[0] != Reference(Input.GetIdentity()))
					return std::unexpected(Error("Physics captured identity changed."));
				const auto Identity = HashInput(Input.GetPositions().GetBytes(), Input.GetIndices().GetBytes(), [&] { return Cancel.IsCancelled(); });
				if (!Identity) return std::unexpected(Cancelled());
				if (*Identity != Input.GetIdentity()) return std::unexpected(Error("Physics captured bytes do not match their identity."));
				return std::vector<FBuildInput>{{.Identity = Inputs[0],
					.Values = {{"Positions", Input.GetPositions()}, {"Indices", Input.GetIndices()}}}};
			}
		private:
			FPhysicsCookInput Input;
		};
		class FFunction final : public IBuildFunction
		{
		public:
			auto GetDescriptor() const -> FBuildFunctionDescriptor override { return GetPhysicsCookBuildDescriptor(); }
			auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildFailure> override
			{
				bool bCancelled = false;
				const auto Cancel = [&] { return bCancelled = bCancelled || Context.IsCancelled(); };
				auto Config = Settings(Context.GetAction()); if (!Config) return std::unexpected(std::move(Config.error()));
				const auto Inputs = Context.GetInputs();
				if (Inputs.size() != 1 || Inputs[0].Identity != Reference(Inputs[0].Identity.Identity)
					|| !Inputs[0].Metadata.IsEmpty() || Inputs[0].Values.size() != 2)
					return std::unexpected(Error("Physics source representation is invalid."));
				const FSharedByteBuffer *Positions = nullptr, *Indices = nullptr;
				for (const auto& Value : Inputs[0].Values)
				{
					if (Value.Id == "Positions") Positions = &Value.Data;
					if (Value.Id == "Indices") Indices = &Value.Data;
				}
				if (!Positions || !Indices || Positions->size() % sizeof(FVector3f) || Indices->size() % sizeof(uint32))
					return std::unexpected(Error("Physics source arrays are malformed."));
				const auto PositionCount = Positions->size() / sizeof(FVector3f), IndexCount = Indices->size() / sizeof(uint32);
				if (auto Valid = CheckCookInput(PositionCount, IndexCount, Config->Mode, Config->Policy, Context.GetMaximumWorkingSetBytes()); !Valid)
					return std::unexpected(Error(Valid.error().ToString()));
				const auto Identity = HashInput(Positions->GetBytes(), Indices->GetBytes(), Cancel);
				if (!Identity) return std::unexpected(Cancelled());
				if (*Identity != Inputs[0].Identity.Identity) return std::unexpected(Error("Physics source semantic identity is invalid."));
				std::vector<FVector3> ConvertedPositions; ConvertedPositions.reserve(PositionCount);
				for (size_t Index = 0; Index < PositionCount; ++Index)
				{
					if (Index % 256 == 0 && Cancel()) return std::unexpected(Cancelled());
					FVector3f Position; std::memcpy(&Position, Positions->data() + Index * sizeof(Position), sizeof(Position));
					ConvertedPositions.emplace_back(Position);
				}
				std::vector<uint32> ConvertedIndices;
				auto NativeIndices = Indices->GetNativeView<uint32>();
				if (!NativeIndices)
				{
					ConvertedIndices.resize(IndexCount);
					for (size_t Index = 0; Index < IndexCount; ++Index)
					{
						if (Index % 256 == 0 && Cancel()) return std::unexpected(Cancelled());
						std::memcpy(&ConvertedIndices[Index], Indices->data() + Index * sizeof(uint32), sizeof(uint32));
					}
					NativeIndices = std::span<const uint32>(ConvertedIndices);
				}
				FCollisionGeometryBuildDiagnostics Diagnostics;
				auto Cooked = Config->Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
					? FCollisionCookedData::BuildConvexHull(ConvertedPositions, &Diagnostics, Cancel)
					: FCollisionCookedData::BuildTriangleMesh(ConvertedPositions, *NativeIndices, &Diagnostics, Cancel);
				if (Cancel() || Diagnostics.Status == ECollisionGeometryBuildStatus::Cancelled) return std::unexpected(Cancelled());
				if (!Cooked) return std::unexpected(Error(std::format("Physics geometry construction failed (status {}).", int(Diagnostics.Status)), EBuildFailureReason::ProducerFailure));
				Context.ReportMetric("Physics.FloatToDoubleRecipeBytes", PositionCount * sizeof(FVector3));
				auto Output = MakeSharedOutput(std::move(Cooked), Config->Mode, Config->Policy, Cancel);
				if (Cancel()) return std::unexpected(Cancelled());
				if (!Output) return std::unexpected(Error(std::move(Output.error()), EBuildFailureReason::InvalidOutput));
				return std::move(*Output);
			}
			auto Validate(const FBuildAction& Action, const FBuildOutput& Output, const FBuildCancellation& Cancellation) const
				-> FBuildValidationResult override
			{
				auto Config = Settings(Action); if (!Config) return std::unexpected(std::move(Config.error()));
				bool bCancelled = false;
				const auto Cancel = [&] { return bCancelled = bCancelled || Cancellation.IsCancelled(); };
				auto Valid = ValidateSharedOutput(Output, Config->Mode, Config->Policy, Cancel);
				if (Cancel()) return std::unexpected(Cancelled());
				if (!Valid) return std::unexpected(Error(std::move(Valid.error()), EBuildFailureReason::InvalidOutput));
				return {};
			}
		};
	}
	auto MakeCollisionBuildFunction() -> std::shared_ptr<const IBuildFunction> { return std::make_shared<FFunction>(); }
	auto MakeCollisionInputResolver(FPhysicsCookInput Input) -> std::shared_ptr<const IBuildInputResolver>
	{ return std::make_shared<FResolver>(std::move(Input)); }
}
#endif
