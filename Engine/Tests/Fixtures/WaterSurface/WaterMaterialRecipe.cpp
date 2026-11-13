#include "WaterMaterialRecipe.h"
#include "WaterSurfaceTypes.h"

namespace Durin::WaterMaterialRecipe
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

	}

	auto BuildInput() -> MIR::FCompilerInput
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

}
