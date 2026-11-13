#include "WaterSurfaceResources.h"

#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialShaderMap.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		using Op = EMaterialProgramOpcode;
		using Type = EMaterialProgramValueType;
		class FWaterMaterialBuilder
		{
		public:
			MIR::FCompilerInput Input;
			auto Add(MIR::FNode Node) -> uint32
			{
				if (Node.Opcode != Op::Constant)
				{
					std::vector<FMaterialValueSemantics> Arguments;
					for (uint32 Slot = 0; Slot < Node.Inputs.size(); ++Slot)
					{
						auto V = Input.IR.Nodes[Node.Inputs[Slot]].GetSemantics();
						if (Node.ScalarBroadcastMask & (1u << Slot)) V.Type = Type::Float;
						Arguments.push_back(V);
					}
					const auto* Transform = std::get_if<FMaterialTransformPayload>(&Node.Payload);
					const auto S = ResolveMaterialProgramNodeSemantics(Node.Opcode, Node.ResultType, Arguments, Transform);
					check(S.has_value());
					Node.LegalStages = S->Stages; Node.SpatialKind = S->Kind; Node.CoordinateSpace = S->Space;
				}
				Input.IR.Nodes.push_back(std::move(Node));
				return uint32(Input.IR.Nodes.size() - 1);
			}
			auto C(float V) -> uint32 { return Add({.Opcode = Op::Constant, .Payload = FMaterialProgramLiteral{V}}); }
			auto U(Op Code, uint32 A, Type T = Type::Float) -> uint32 { return Add({.Opcode = Code, .ResultType = T, .Inputs = {A}}); }
			auto B(Op Code, uint32 A, uint32 B, Type T = Type::Float) -> uint32 { return Add({.Opcode = Code, .ResultType = T, .Inputs = {A, B}}); }
			auto V3(uint32 X, uint32 Y, uint32 Z) -> uint32 { return Add({.Opcode = Op::MakeFloat3, .ResultType = Type::Float3, .Inputs = {X, Y, Z}}); }
			auto Scale(uint32 Vector, uint32 Scalar) -> uint32
			{
				return Add({.Opcode = Op::Multiply, .ResultType = Type::Float3, .ScalarBroadcastMask = 2,
					.Inputs = {Vector, U(Op::Splat3, Scalar, Type::Float3)}});
			}
		};

		auto Failure(const FMaterialCompilerResult& P) -> std::string
		{ return P.Diagnostics.empty() ? "Water material compilation failed." : P.Diagnostics.front().Message; }
	}

	auto MakeWaterMaterialInput() -> MIR::FCompilerInput
	{
		FWaterMaterialBuilder B;
		FMaterialSurfaceOutputs Defaults;
		for (uint32 I = 0; I < 8; ++I)
		{
			auto& Root = B.Input.IR.SurfaceRoot.Inputs[I];
			const auto S = GetMaterialSurfaceOutputSemantics(static_cast<EMaterialSurfaceOutput>(I));
			Root.Type = S.Type; Root.LegalStages = S.Stages; Root.SpatialKind = S.Kind; Root.CoordinateSpace = S.Space;
			Root.Literal = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(I));
		}
		B.Input.StaticProperties.bTwoSided = true;
		B.Input.IR.SurfaceRoot.Inputs[size_t(EMaterialSurfaceOutput::BaseColor)].Literal = {.02f, .18f, .24f};
		B.Input.IR.SurfaceRoot.Inputs[size_t(EMaterialSurfaceOutput::Roughness)].Literal = {.22f};
		const auto Zero = B.C(0), One = B.C(1);
		const auto UV = B.Add({.Opcode = Op::UVChannel, .ResultType = Type::Float2, .Inputs = {Zero}});
		auto Channel = [&](uint8 I) { return B.Add({.Opcode = Op::Swizzle, .Inputs = {UV}, .Payload = MIR::FSwizzle{1, {I}}}); };
		const auto X = Channel(0), Y = Channel(1), Time = B.Add({.Opcode = Op::Time});
		std::array<uint32, 4> Parameters;
		for (uint32 I = 0; I < 4; ++I) Parameters[I] = B.Add({.Opcode = Op::PrimitiveCustomData, .Payload = MIR::FCustomDataRead{I}});
		const auto BaseK = B.B(Op::Divide, B.C(6.28318530718f), Parameters[1]);
		uint32 DX = Zero, DY = Zero, DZ = Zero, TXX = One, TXY = Zero, TXZ = Zero, TYY = One, TYZ = Zero;
		for (const auto& Wave : WaterWaves)
		{
			const auto A = B.B(Op::Multiply, Parameters[0], B.C(Wave.AmplitudeRatio));
			const auto K = B.B(Op::Multiply, BaseK, B.C(Wave.WavenumberRatio));
			const auto QA = B.B(Op::Multiply, Parameters[3], A);
			const auto DirectionX = B.C(float(Wave.Direction.x)), DirectionY = B.C(float(Wave.Direction.y));
			const auto Dot = B.B(Op::Add, B.B(Op::Multiply, X, DirectionX), B.B(Op::Multiply, Y, DirectionY));
			const auto Omega = B.B(Op::Multiply, Parameters[2], B.U(Op::Sqrt, B.B(Op::Multiply, B.C(9.81f), K)));
			const auto Phase = B.B(Op::Subtract, B.B(Op::Multiply, K, Dot), B.B(Op::Multiply, Omega, Time));
			const auto Sin = B.U(Op::Sine, Phase), Cos = B.U(Op::Cosine, Phase);
			const auto Horizontal = B.B(Op::Multiply, QA, Cos), Derivative = B.B(Op::Multiply, B.B(Op::Multiply, QA, K), Sin);
			DX = B.B(Op::Add, DX, B.B(Op::Multiply, Horizontal, DirectionX));
			DY = B.B(Op::Add, DY, B.B(Op::Multiply, Horizontal, DirectionY));
			DZ = B.B(Op::Add, DZ, B.B(Op::Multiply, A, Sin));
			TXX = B.B(Op::Subtract, TXX, B.B(Op::Multiply, Derivative, B.C(float(Wave.Direction.x * Wave.Direction.x))));
			TXY = B.B(Op::Subtract, TXY, B.B(Op::Multiply, Derivative, B.C(float(Wave.Direction.x * Wave.Direction.y))));
			TYY = B.B(Op::Subtract, TYY, B.B(Op::Multiply, Derivative, B.C(float(Wave.Direction.y * Wave.Direction.y))));
			const auto Vertical = B.B(Op::Multiply, B.B(Op::Multiply, A, K), Cos);
			TXZ = B.B(Op::Add, TXZ, B.B(Op::Multiply, Vertical, DirectionX));
			TYZ = B.B(Op::Add, TYZ, B.B(Op::Multiply, Vertical, DirectionY));
		}
		uint32 Offset = 0;
		const std::array Components{DX, DY, DZ};
		for (uint32 I = 0; I < 3; ++I)
		{
			FMaterialProgramLiteral Axis{};
			if (I == 0) Axis.X = 1; else if (I == 1) Axis.Y = 1; else Axis.Z = 1;
			const auto LocalAxis = B.Add({.Opcode = Op::Constant, .ResultType = Type::Float3,
				.SpatialKind = EMaterialSpatialKind::Direction, .CoordinateSpace = EMaterialCoordinateSpace::Object, .Payload = Axis});
			const auto WorldAxis = B.Add({.Opcode = Op::TransformDirection, .ResultType = Type::Float3, .Inputs = {LocalAxis},
				.Payload = FMaterialTransformPayload{EMaterialCoordinateSpace::Object, EMaterialCoordinateSpace::World}});
			const auto Term = B.Scale(WorldAxis, Components[I]);
			Offset = I ? B.B(Op::Add, Offset, Term, Type::Float3) : Term;
		}
		B.Input.IR.WorldPositionOffset.bExpression = true; B.Input.IR.WorldPositionOffset.ExpressionIndex = Offset;
		const auto Normal = B.U(Op::Normalize, B.B(Op::Cross, B.V3(TXX, TXY, TXZ), B.V3(TXY, TYY, TYZ), Type::Float3), Type::Float3);
		const auto XY = B.Add({.Opcode = Op::Swizzle, .ResultType = Type::Float2, .Inputs = {Normal}, .Payload = MIR::FSwizzle{2, {0, 1}}});
		const auto Half = B.Add({.Opcode = Op::Constant, .ResultType = Type::Float2, .Payload = FMaterialProgramLiteral{.5f, .5f}});
		const auto Encoded = B.B(Op::Add, B.B(Op::Multiply, XY, Half, Type::Float2), Half, Type::Float2);
		auto& Root = B.Input.IR.SurfaceRoot.Inputs[size_t(EMaterialSurfaceOutput::Normal)];
		Root.bExpression = true; Root.ExpressionIndex = B.U(Op::DecodeNormalRG, Encoded, Type::Float3);
		return std::move(B.Input);
	}

	auto MakeWaterGrid(const FWaterSurfaceSettings& S) -> std::unique_ptr<FStaticMeshRenderData>
	{
		if (!ValidateWaterSurface(S)) return {};
		const double Step = S.Wavelength * .73 / 32;
		const uint32 NX = uint32(std::ceil(S.Extent.x / Step)), NY = uint32(std::ceil(S.Extent.y / Step));
		const uint32 Count = (NX + 1) * (NY + 1);
		auto Data = std::make_unique<FStaticMeshRenderData>();
		Data->MaterialSlots.resize(1); Data->MaterialSlots[0].Name = "Water";
		Data->LODResources.resize(1); Data->LODVertexFactories.resize(1);
		std::vector<FVector3f> Positions;
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> UVs;
		for (uint32 Y = 0; Y <= NY; ++Y) for (uint32 X = 0; X <= NX; ++X)
		{
			const FVector2f P(float(S.Extent.x * (double(X) / NX - .5)), float(S.Extent.y * (double(Y) / NY - .5)));
			Positions.emplace_back(P, S.Elevation); UVs[0].push_back(P);
		}
		std::vector<uint32> Indices;
		for (uint32 Y = 0; Y < NY; ++Y) for (uint32 X = 0; X < NX; ++X)
		{
			const uint32 A = Y * (NX + 1) + X, B = A + NX + 1;
			Indices.insert(Indices.end(), {A, A + 1, B + 1, A, B + 1, B});
		}
		auto& LOD = Data->LODResources[0];
		LOD.VertexBuffers.PositionVertexBuffer.Init(std::move(Positions));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<FVector3f>(Count, {0, 0, 1}), std::vector<FVector4f>(Count, {1, 0, 0, 1}));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(std::move(UVs), Count, 1);
		LOD.VertexBuffers.ColorVertexBuffer.Init(std::vector<FVector4f>(Count, FVector4f(1)), Count);
		LOD.IndexBuffer.Init(std::move(Indices));
		const double Padding = 1.e-4 + 2.e-7 * std::max({std::abs(double(S.Elevation)), S.Extent.x, S.Extent.y});
		const double H = S.Amplitude * S.Steepness + Padding;
		const FBox Bounds({-S.Extent.x * .5 - H, -S.Extent.y * .5 - H, S.Elevation - S.Amplitude - Padding},
			{S.Extent.x * .5 + H, S.Extent.y * .5 + H, S.Elevation + S.Amplitude + Padding});
		LOD.Sections.push_back({.Name = "Water", .FirstIndex = 0, .IndexCount = NX * NY * 6,
			.MinVertexIndex = 0, .MaxVertexIndex = Count - 1, .MaterialSlotIndex = 0, .LocalBounds = Bounds});
		LOD.NumTexCoords = 1; LOD.bHasColorVertexData = true; LOD.LocalBounds = Bounds; Data->LocalBounds = Bounds;
		return Data;
	}

	auto CreateWaterMaterial(ERHIFeatureLevel Feature) -> std::expected<FMaterialRenderProxyRef, std::string>
	{
		FMaterialStaticProperties Properties;
		std::shared_ptr<const FMaterialCompilerResult> Program;
#if DURIN_WITH_EDITOR
		auto Input = MakeWaterMaterialInput();
		if (auto E = BuildDefaultMaterialCompilerEnvironment(Input.Environment); !E) return std::unexpected(E.error());
		Input.Environment.FeatureLevel = Feature; Input.Environment.Quality = GetMaterialQualityLevel();
		Properties = Input.StaticProperties;
		Program = std::make_shared<const FMaterialCompilerResult>(MIR::Compile(Input));
		if (!*Program) return std::unexpected(Failure(*Program));
#else
		const auto Path = FMountPaths::ResolveAssetPath("/Plugins/Water/Compiled/Win64/Water.bin");
		if (!Path) return std::unexpected("Water content mount is unavailable; enable the plugin before startup.");
		const auto Bytes = FFileHelper::LoadFileToArray(Path->PhysicalPath);
		if (!Bytes) return std::unexpected("Water precompiled material is missing; run the water resource compiler in Editor.");
		const auto Result = DecodeMaterialCookedProgramFamily(*Bytes, ECookTargetPlatform::Win64, ECookTargetProfile::Game,
			GetMaterialQualityLevel(), Feature, {}, Properties, Program);
		if (!Result) return std::unexpected("Water precompiled material: " + Result.error());
#endif
		auto Proxy = MakeRefCount<FMaterialRenderProxy>();
		Proxy->QueuePublication_GameThread({.StaticProperties = Properties, .CompiledProgram = std::move(Program)});
		return Proxy;
	}

#if DURIN_WITH_EDITOR
	auto PrecompileWaterMaterial(const std::filesystem::path& OutputDirectory) -> std::expected<void, std::string>
	{
		std::vector<FMaterialCompilerResult> Programs;
		auto Input = MakeWaterMaterialInput();
		if (auto E = BuildDefaultMaterialCompilerEnvironment(Input.Environment); !E) return E;
		Input.Environment.bIncludeEditorPasses = false;
		for (auto Quality : SupportedMaterialQualityLevels) for (auto Feature : SupportedMaterialFeatureLevels)
		{
			Input.Environment.Quality = Quality; Input.Environment.FeatureLevel = Feature;
			auto P = MIR::Compile(Input);
			if (!P) return std::unexpected(Failure(P));
			Programs.push_back(std::move(P));
		}
		std::vector<const FMaterialCompilerResult*> Views;
		for (const auto& P : Programs) Views.push_back(&P);
		FByteBuffer Bytes;
		if (auto E = EncodeMaterialCookedProgramFamily(Views, Input.StaticProperties, ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes); !E) return E;
		std::filesystem::create_directories(OutputDirectory / "Compiled/Win64");
		if (!FFileHelper::SaveArrayToFile(Bytes, OutputDirectory / "Compiled/Win64/Water.bin")) return std::unexpected("Cannot write precompiled Water material.");
		return {};
	}
#endif
}
