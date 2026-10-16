#pragma once

#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/Guid.h"
#include "FunctionReflectionTestTypes.gen.h"

namespace Durin::FunctionFixtures
{
	DENUM()
	enum class EFunctionState : uint8 { Idle = 0, Ready = 7, High = 255 };
	DENUM()
	enum class EOtherFunctionState : uint8 { Other = 7 };
	DENUM()
	enum class EFunctionI8 : int8 { Low = -128 };
	DENUM()
	enum class EFunctionI16 : int16 { Low = -32768 };
	DENUM()
	enum class EFunctionU16 : uint16 { High = 65535 };
	DENUM()
	enum class EFunctionI32 : int32 { Low = -2147483647 - 1 };
	DENUM()
	enum class EFunctionU32 : uint32 { High = 4294967295U };
	DENUM()
	enum class EFunctionI64 : int64 { Low = -9223372036854775807LL - 1 };
	DENUM()
	enum class EFunctionU64 : uint64 { High = 18446744073709551615ULL };
	DENUM()
	enum EFunctionPlain { PlainZero = 0, PlainSeven = 7 };

	DSTRUCT()
	struct FFunctionPayload
	{
		GENERATED_BODY()
		inline static int32 Live = 0;
		FFunctionPayload() { ++Live; }
		FFunctionPayload(const FFunctionPayload& Other) : Text(Other.Text), Objects(Other.Objects) { ++Live; }
		auto operator=(const FFunctionPayload&) -> FFunctionPayload& = default;
		~FFunctionPayload() { --Live; }
		DPROPERTY()
		std::string Text;
		DPROPERTY()
		std::vector<Durin::TObjectPtr<Durin::DObject>> Objects;
	};

	DSTRUCT()
	struct FOtherFunctionPayload
	{
		GENERATED_BODY()
		DPROPERTY()
		std::string Text;
		DPROPERTY()
		std::vector<Durin::TObjectPtr<Durin::DObject>> Objects;
	};

	// Exercises actual generated native storage, reference binding, and return assignment.
	DCLASS(NoClassDefaultObject)
	class DParameterTarget : public Durin::DObject
	{
		GENERATED_BODY()
	public:
		explicit DParameterTarget(const Durin::FObjectInitializer& Initializer) : DObject(Initializer) {}
		DFUNCTION()
		auto Combine(int32 Value, const std::string& Prefix, std::string& Output) const -> std::string
		{
			Output = Prefix + std::to_string(Value);
			return Output + "!";
		}
		DFUNCTION()
		virtual auto Scale(int32 Value) -> int32 { return Value * 2; }
		DFUNCTION()
		auto EchoGuid(const Durin::FGuid& Value) const noexcept -> Durin::FGuid { return Value; }
		DFUNCTION()
		auto EchoName(Durin::FName Value) const -> Durin::FName { return Value; }
		DFUNCTION()
		auto ReadValue() const -> int32 { return 17; }
		DFUNCTION()
		auto Update(int32& Value) noexcept -> void { Value += 5; }
		DFUNCTION()
		auto Mixed(float Left, double Right, bool Negate) const -> double
		{ return (Left + Right) * (Negate ? -1.0 : 1.0); }
		DFUNCTION()
		auto EchoObject(Durin::DObject* Value) -> Durin::DObject* { return Value; }
		DFUNCTION()
		auto ReplaceObject(Durin::DObject*& Value, Durin::DObject* const& Replacement) -> Durin::DObject*
		{ auto* Previous = Value; Value = Replacement; return Previous; }
		DFUNCTION()
		auto EchoWrapped(Durin::TObjectPtr<Durin::DObject> Value) -> Durin::TObjectPtr<Durin::DObject> { return Value; }
		DFUNCTION()
		auto EchoObjects(const std::vector<Durin::DObject*>& Values) -> std::vector<Durin::DObject*> { return Values; }
		DFUNCTION()
		auto EchoTransform(const Durin::FTransform& Value) -> Durin::FTransform { return Value; }
		DFUNCTION()
		virtual auto Transform(const FFunctionPayload& Input, FFunctionPayload& Output) -> FFunctionPayload
		{ Output = Input; Output.Text += " output"; return Output; }
		DFUNCTION()
		auto TransformMap(const std::unordered_map<std::string, std::vector<FFunctionPayload>>& Input,
			std::unordered_map<std::string, std::vector<FFunctionPayload>>& Output)
			-> std::unordered_map<std::string, std::vector<FFunctionPayload>>
		{ Output = Input; Output["result"] = Input.at("input"); return Output; }
		DFUNCTION()
		auto ThrowPayload(FFunctionPayload& Output) -> FFunctionPayload
		{ Output.Text = "changed"; throw std::runtime_error("native payload error"); }
		DFUNCTION()
		auto CollectDuringCall(std::vector<Durin::DObject*> Values) -> std::vector<Durin::DObject*>
		{ Durin::CollectGarbage(); return Values; }
		DFUNCTION()
		virtual auto EchoState(EFunctionState Value, EFunctionState& Output) -> EFunctionState { Output = Value; return Value; }
		DFUNCTION()
		auto EchoI8(EFunctionI8 Value) -> EFunctionI8 { return Value; }
		DFUNCTION()
		auto EchoI16(EFunctionI16 Value) -> EFunctionI16 { return Value; }
		DFUNCTION()
		auto EchoU16(EFunctionU16 Value) -> EFunctionU16 { return Value; }
		DFUNCTION()
		auto EchoI32(EFunctionI32 Value) -> EFunctionI32 { return Value; }
		DFUNCTION()
		auto EchoU32(EFunctionU32 Value) -> EFunctionU32 { return Value; }
		DFUNCTION()
		auto EchoI64(EFunctionI64 Value) -> EFunctionI64 { return Value; }
		DFUNCTION()
		auto EchoU64(EFunctionU64 Value) -> EFunctionU64 { return Value; }
		DFUNCTION()
		auto EchoPlain(EFunctionPlain Value) -> EFunctionPlain { return Value; }
		DFUNCTION()
		virtual auto EchoSoft(const Durin::TSoftObjectPtr<Durin::DObject>& Input,
			Durin::TSoftObjectPtr<Durin::DObject>& Output) -> Durin::TSoftObjectPtr<Durin::DObject>
		{ Output = Input; return Input; }
		DFUNCTION()
		virtual auto EchoWeak(Durin::TWeakObjectPtr<Durin::DObject> Input,
			Durin::TWeakObjectPtr<Durin::DObject>& Output) -> Durin::TWeakObjectPtr<Durin::DObject>
		{ Output = Input; return Input; }
		DFUNCTION()
		auto EchoSoftMap(const std::unordered_map<EFunctionState, std::vector<Durin::TSoftObjectPtr<Durin::DObject>>>& Input)
			-> std::unordered_map<EFunctionState, std::vector<Durin::TSoftObjectPtr<Durin::DObject>>> { return Input; }
		DFUNCTION()
		auto EchoWeakArray(const std::vector<Durin::TWeakObjectPtr<Durin::DObject>>& Input)
			-> std::vector<Durin::TWeakObjectPtr<Durin::DObject>> { return Input; }
	};

	// Parameter names do not participate in reflected override compatibility.
	DCLASS(NoClassDefaultObject)
	class DParameterDerived : public DParameterTarget
	{
		GENERATED_BODY()
	public:
		explicit DParameterDerived(const Durin::FObjectInitializer& Initializer) : DParameterTarget(Initializer) {}
		DFUNCTION()
		auto Scale(int32 Renamed) -> int32 override { return Renamed * 3; }
		DFUNCTION()
		auto Transform(const FFunctionPayload& Input, FFunctionPayload& Renamed) -> FFunctionPayload override
		{ Renamed = Input; Renamed.Text += " derived"; return Renamed; }
		DFUNCTION()
		auto EchoState(EFunctionState Renamed, EFunctionState& Output) -> EFunctionState override { Output = Renamed; return EFunctionState::Ready; }
		DFUNCTION()
		auto EchoSoft(const Durin::TSoftObjectPtr<Durin::DObject>& Input,
			Durin::TSoftObjectPtr<Durin::DObject>& Renamed) -> Durin::TSoftObjectPtr<Durin::DObject> override
		{ Renamed = Input; return Input; }
		DFUNCTION()
		auto EchoWeak(Durin::TWeakObjectPtr<Durin::DObject> Input,
			Durin::TWeakObjectPtr<Durin::DObject>& Renamed) -> Durin::TWeakObjectPtr<Durin::DObject> override
		{ Renamed = Input; return Input; }
	};
}
