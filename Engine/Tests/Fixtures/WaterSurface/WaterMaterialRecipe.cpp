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
		B.Input.StaticProperties.bTwoSided = true;
		B.Input.StaticProperties.ShadingModel = EMaterialShadingModel::SingleLayerWater;
		B.Input.StaticProperties.RefractionMethod = EMaterialRefractionMethod::PixelNormalOffset;
		B.Input.IR.GetAttribute(EMaterialProperty::MP_BaseColor).Literal = {.02f, .18f, .24f};
		B.Input.IR.GetAttribute(EMaterialProperty::MP_Roughness).Literal = {.22f};
		B.Input.IR.GetAttribute(EMaterialProperty::MP_Opacity).Literal = {0.f};
		const auto WorldPosition = B.Add({.Opcode = Op::WorldPosition, .ResultType = Type::Float3});
		const auto ViewPosition = B.Add({.Opcode = Op::TransformPosition, .ResultType = Type::Float3, .Inputs = {WorldPosition},
			.Payload = FMaterialTransformPayload{EMaterialCoordinateSpace::World, EMaterialCoordinateSpace::View}});
		const auto ViewDepth = B.Add({.Opcode = Op::Swizzle, .Inputs = {ViewPosition}, .Payload = MIR::FSwizzle{1, {0}}});
		const auto DepthFraction = B.U(Op::Saturate, B.B(Op::Divide, ViewDepth, B.C(50.f)));
		auto& Refraction = B.Input.IR.GetAttribute(EMaterialProperty::MP_Refraction);
		Refraction.bExpression = true;
		Refraction.ExpressionIndex = B.Add({.Opcode = Op::Lerp, .Inputs = {B.C(1.15f), B.C(1.02f), DepthFraction}});
		const auto Zero = B.C(0), One = B.C(1);
		const auto UV = B.Add({.Opcode = Op::UVChannel, .ResultType = Type::Float2, .Inputs = {Zero}});
		auto Channel = [&](uint8 I) { return B.Add({.Opcode = Op::Swizzle, .Inputs = {UV}, .Payload = MIR::FSwizzle{1, {I}}}); };
		const auto X = Channel(0), Y = Channel(1), Time = B.Add({.Opcode = Op::Time});
		const auto ReadParameter = [&](uint32 Index) {
			return B.Add({.Opcode = Op::PrimitiveCustomData, .Payload = MIR::FCustomDataRead{Index}});
		};
		std::array<uint32, WaterWaveDataStart> Parameters;
		for (uint32 I = 0; I < Parameters.size(); ++I) Parameters[I] = ReadParameter(I);
		const auto BaseK = B.B(Op::Divide, B.C(6.28318530718f), Parameters[1]);
		const auto Gravity = B.C(9.81f);
		uint32 DX = Zero, DY = Zero, DZ = Zero, TXX = One, TXY = Zero, TXZ = Zero, TYY = One, TYZ = Zero;
		for (uint32 I = 0; I < MaxWaterWaves; ++I)
		{
			uint32 DirectionX, DirectionY, Weight, Frequency;
			if (I < PrimitiveWaterWaves)
			{
				const uint32 Slot = WaterWaveDataStart + I * WaterWaveDataStride;
				DirectionX = ReadParameter(Slot);
				DirectionY = ReadParameter(Slot + 1);
				Weight = ReadParameter(Slot + 2);
				Frequency = ReadParameter(Slot + 3);
			}
			else
			{
				const uint32 Slot = (I - PrimitiveWaterWaves) * InstanceWaterWaveStride;
				auto Instance = [&](uint32 Index) {
					return B.Add({.Opcode = Op::InstanceCustomData, .Payload = MIR::FCustomDataRead{Index}});
				};
				const auto Angle = Instance(Slot);
				DirectionX = B.U(Op::Cosine, Angle);
				DirectionY = B.U(Op::Sine, Angle);
				Weight = Instance(Slot + 1);
				Frequency = Instance(Slot + 2);
			}
			const auto A = B.B(Op::Multiply, Parameters[0], Weight);
			const auto K = B.B(Op::Multiply, BaseK, Frequency);
			const auto QA = B.B(Op::Multiply, Parameters[3], A);
			const auto Dot = B.B(Op::Add, B.B(Op::Multiply, X, DirectionX), B.B(Op::Multiply, Y, DirectionY));
			const auto Omega = B.B(Op::Multiply, Parameters[2], B.U(Op::Sqrt, B.B(Op::Multiply, Gravity, K)));
			const auto Phase = B.B(Op::Subtract, B.B(Op::Multiply, K, Dot), B.B(Op::Multiply, Omega, Time));
			const auto Sin = B.U(Op::Sine, Phase), Cos = B.U(Op::Cosine, Phase);
			const auto Horizontal = B.B(Op::Multiply, QA, Cos), Derivative = B.B(Op::Multiply, B.B(Op::Multiply, QA, K), Sin);
			DX = B.B(Op::Add, DX, B.B(Op::Multiply, Horizontal, DirectionX));
			DY = B.B(Op::Add, DY, B.B(Op::Multiply, Horizontal, DirectionY));
			DZ = B.B(Op::Add, DZ, B.B(Op::Multiply, A, Sin));
			TXX = B.B(Op::Subtract, TXX, B.B(Op::Multiply, Derivative, B.B(Op::Multiply, DirectionX, DirectionX)));
			TXY = B.B(Op::Subtract, TXY, B.B(Op::Multiply, Derivative, B.B(Op::Multiply, DirectionX, DirectionY)));
			TYY = B.B(Op::Subtract, TYY, B.B(Op::Multiply, Derivative, B.B(Op::Multiply, DirectionY, DirectionY)));
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
		B.Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).bExpression = true; B.Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex = Offset;
		const auto Normal = B.U(Op::Normalize, B.B(Op::Cross, B.V3(TXX, TXY, TXZ), B.V3(TXY, TYY, TYZ), Type::Float3), Type::Float3);
		const auto XY = B.Add({.Opcode = Op::Swizzle, .ResultType = Type::Float2, .Inputs = {Normal}, .Payload = MIR::FSwizzle{2, {0, 1}}});
		const auto Half = B.Add({.Opcode = Op::Constant, .ResultType = Type::Float2, .Payload = FMaterialProgramLiteral{.5f, .5f}});
		const auto Encoded = B.B(Op::Add, B.B(Op::Multiply, XY, Half, Type::Float2), Half, Type::Float2);
		auto& Root = B.Input.IR.GetAttribute(EMaterialProperty::MP_Normal);
		Root.bExpression = true; Root.ExpressionIndex = B.U(Op::DecodeNormalRG, Encoded, Type::Float3);
		auto SetWaterOutput = [&](FGuid Slot, Type ResultType, FMaterialProgramLiteral Value) {
			auto& Output = B.Input.IR.GetCustomOutput(Slot);
			Output.bExpression = true;
			Output.ExpressionIndex = B.Add({.Opcode = Op::Constant, .ResultType = ResultType, .Payload = Value});
		};
		SetWaterOutput(SingleLayerWater::Scattering, Type::Float3, {.00035f, .00065f, .0009f});
		SetWaterOutput(SingleLayerWater::Absorption, Type::Float3, {.004f, .0012f, .00065f});
		SetWaterOutput(SingleLayerWater::PhaseG, Type::Float, {.2f});
		SetWaterOutput(SingleLayerWater::ColorScale, Type::Float3, {1.f, 1.f, 1.f});
		return std::move(B.Input);
	}

}
