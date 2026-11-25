#include "MaterialExpressionGerstnerWaves.h"

#include "Materials/MaterialExpressionBuild.h"
#include "WaterSurfaceTypes.h"

namespace Durin
{
	namespace
	{
		using Op = EMaterialProgramOpcode;
		using Type = EMaterialProgramValueType;
		struct FWaveEmitter
		{
			MIR::FEmitter& Emitter;
			auto Add(MIR::FNode Node) -> uint32 { return Emitter.Emit(std::move(Node)); }
			auto Constant(float V) -> uint32 { return Add({.Opcode = Op::Constant, .Payload = FMaterialProgramLiteral{V}}); }
			auto Unary(Op Code, uint32 A, Type T = Type::Float) -> uint32 { return Add({.Opcode = Code, .ResultType = T, .Inputs = {A}}); }
			auto Binary(Op Code, uint32 A, uint32 B, Type T = Type::Float) -> uint32 { return Add({.Opcode = Code, .ResultType = T, .Inputs = {A, B}}); }
			auto Vector3(uint32 X, uint32 Y, uint32 Z) -> uint32 { return Add({.Opcode = Op::MakeFloat3, .ResultType = Type::Float3, .Inputs = {X, Y, Z}}); }
			auto Scale(uint32 Vector, uint32 Scalar) -> uint32
			{
				return Add({.Opcode = Op::Multiply, .ResultType = Type::Float3, .ScalarBroadcastMask = 2,
					.Inputs = {Vector, Unary(Op::Splat3, Scalar, Type::Float3)}});
			}
		};
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterialExpressionGerstnerWaves::GetOutputs() const -> std::span<const FMaterialExpressionOutput>
	{
		static constexpr FMaterialExpressionOutput Outputs[]{
			{"World Displacement", Type::Float3}, {"Tangent Normal", Type::Float3}};
		return Outputs;
	}
#endif

	auto DMaterialExpressionGerstnerWaves::Build(MIR::FEmitter& Emitter) const -> void
	{
		FWaveEmitter Wave{Emitter};
		const auto Zero = Wave.Constant(0), One = Wave.Constant(1);
		const auto UV = Wave.Add({.Opcode = Op::UVChannel, .ResultType = Type::Float2, .Inputs = {Zero}});
		const auto Channel = [&](uint8 Index) {
			return Wave.Add({.Opcode = Op::Swizzle, .Inputs = {UV}, .Payload = MIR::FSwizzle{1, {Index}}});
		};
		const auto X = Channel(0), Y = Channel(1), Time = Wave.Add({.Opcode = Op::Time});
		const auto ReadParameter = [&](uint32 Index) {
			return Wave.Add({.Opcode = Op::PrimitiveCustomData, .Payload = MIR::FCustomDataRead{Index}});
		};
		const auto BaseAmplitude = ReadParameter(0);
		// Ordinary mesh previews have zeroed water parameters. Keep the zero-amplitude
		// displacement finite when the component has supplied no wavelength.
		const auto BaseWavelength = Wave.Binary(Op::Maximum, ReadParameter(1), Wave.Constant(.0001f));
		const auto Speed = ReadParameter(2);
		const auto Steepness = ReadParameter(3);
		const auto BaseK = Wave.Binary(Op::Divide, Wave.Constant(6.28318530718f), BaseWavelength);
		const auto Gravity = Wave.Constant(9.81f);
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
					return Wave.Add({.Opcode = Op::InstanceCustomData, .Payload = MIR::FCustomDataRead{Index}});
				};
				const auto Angle = Instance(Slot);
				DirectionX = Wave.Unary(Op::Cosine, Angle);
				DirectionY = Wave.Unary(Op::Sine, Angle);
				Weight = Instance(Slot + 1);
				Frequency = Instance(Slot + 2);
			}
			const auto A = Wave.Binary(Op::Multiply, BaseAmplitude, Weight);
			const auto K = Wave.Binary(Op::Multiply, BaseK, Frequency);
			const auto QA = Wave.Binary(Op::Multiply, Steepness, A);
			const auto Dot = Wave.Binary(Op::Add, Wave.Binary(Op::Multiply, X, DirectionX), Wave.Binary(Op::Multiply, Y, DirectionY));
			const auto Omega = Wave.Binary(Op::Multiply, Speed, Wave.Unary(Op::Sqrt, Wave.Binary(Op::Multiply, Gravity, K)));
			const auto Phase = Wave.Binary(Op::Subtract, Wave.Binary(Op::Multiply, K, Dot), Wave.Binary(Op::Multiply, Omega, Time));
			const auto Sin = Wave.Unary(Op::Sine, Phase), Cos = Wave.Unary(Op::Cosine, Phase);
			const auto Horizontal = Wave.Binary(Op::Multiply, QA, Cos), Derivative = Wave.Binary(Op::Multiply, Wave.Binary(Op::Multiply, QA, K), Sin);
			DX = Wave.Binary(Op::Add, DX, Wave.Binary(Op::Multiply, Horizontal, DirectionX));
			DY = Wave.Binary(Op::Add, DY, Wave.Binary(Op::Multiply, Horizontal, DirectionY));
			DZ = Wave.Binary(Op::Add, DZ, Wave.Binary(Op::Multiply, A, Sin));
			TXX = Wave.Binary(Op::Subtract, TXX, Wave.Binary(Op::Multiply, Derivative, Wave.Binary(Op::Multiply, DirectionX, DirectionX)));
			TXY = Wave.Binary(Op::Subtract, TXY, Wave.Binary(Op::Multiply, Derivative, Wave.Binary(Op::Multiply, DirectionX, DirectionY)));
			TYY = Wave.Binary(Op::Subtract, TYY, Wave.Binary(Op::Multiply, Derivative, Wave.Binary(Op::Multiply, DirectionY, DirectionY)));
			const auto Vertical = Wave.Binary(Op::Multiply, Wave.Binary(Op::Multiply, A, K), Cos);
			TXZ = Wave.Binary(Op::Add, TXZ, Wave.Binary(Op::Multiply, Vertical, DirectionX));
			TYZ = Wave.Binary(Op::Add, TYZ, Wave.Binary(Op::Multiply, Vertical, DirectionY));
		}
		uint32 Offset = 0;
		const std::array Components{DX, DY, DZ};
		for (uint32 I = 0; I < 3; ++I)
		{
			FMaterialProgramLiteral Axis{};
			if (I == 0) Axis.X = 1; else if (I == 1) Axis.Y = 1; else Axis.Z = 1;
			const auto LocalAxis = Wave.Add({.Opcode = Op::Constant, .ResultType = Type::Float3,
				.SpatialKind = EMaterialSpatialKind::Direction, .CoordinateSpace = EMaterialCoordinateSpace::Object, .Payload = Axis});
			const auto WorldAxis = Wave.Add({.Opcode = Op::TransformDirection, .ResultType = Type::Float3, .Inputs = {LocalAxis},
				.Payload = FMaterialTransformPayload{EMaterialCoordinateSpace::Object, EMaterialCoordinateSpace::World}});
			const auto Term = Wave.Scale(WorldAxis, Components[I]);
			Offset = I ? Wave.Binary(Op::Add, Offset, Term, Type::Float3) : Term;
		}
		const auto Normal = Wave.Unary(Op::Normalize, Wave.Binary(Op::Cross, Wave.Vector3(TXX, TXY, TXZ), Wave.Vector3(TXY, TYY, TYZ), Type::Float3), Type::Float3);
		Emitter.Output(0, Offset);
		Emitter.Output(1, Wave.Add({.Opcode = Op::Swizzle, .ResultType = Type::Float3,
			.SpatialKind = EMaterialSpatialKind::Normal, .CoordinateSpace = EMaterialCoordinateSpace::Tangent,
			.Inputs = {Normal}, .Payload = MIR::FSwizzle{3, {0, 1, 2}}}));
	}
}
