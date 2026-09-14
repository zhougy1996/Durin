#pragma once

#include "EngineAPI.h"
#include "Input/InputCoreTypes.h"
#include "Math/Vector.h"

namespace Durin
{
	class FGameInputState;

	// Values remain device-independent; delta axes are frame samples, not rates.
	enum class EInputActionType : uint8 { Button, Axis1D, Axis2D };
	enum class EInputSourceKind : uint8 { Key, MouseButton, MouseX, MouseY, MouseWheel };

	// Stable failure categories for binding operations; diagnostics are not a parsing API.
	enum class EInputBindingError : uint8
	{
		None,
		UnknownContext,
		UnknownSlot,
		InvalidSource,
		ReservedSource,
		BindingConflict,
		UnsupportedFormat,
		InvalidFile,
		ReadFailed,
		WriteFailed
	};

	// Context/Slot identify a failing binding when available. For a direct Rebind
	// conflict, Slot is the requested slot and ConflictingSlot is its existing peer.
	// File-wide failures leave binding identities empty. Message is diagnostic only.
	struct [[nodiscard]] FInputBindingResult
	{
		EInputBindingError Error = EInputBindingError::None;
		std::string Message;
		std::string Context;
		std::string Slot;
		std::string ConflictingSlot;

		explicit operator bool() const { return Error == EInputBindingError::None; }
	};

	// A physical source can be reserved independently of the action it drives.
	struct FInputSource
	{
		EInputSourceKind Kind = EInputSourceKind::Key;
		uint16 Code = 0;
		static auto Key(EKey Key) -> FInputSource { return {EInputSourceKind::Key, static_cast<uint16>(Key)}; }
		static auto Mouse(EMouseButton Button) -> FInputSource { return {EInputSourceKind::MouseButton, static_cast<uint16>(Button)}; }
		auto operator==(const FInputSource&) const -> bool = default;
		ENGINE_API auto IsValid() const -> bool;
		auto GetIndex() const -> size_t { return Kind == EInputSourceKind::Key ? Code : Kind == EInputSourceKind::MouseButton ? 256 + Code : 259 + static_cast<size_t>(Kind) - 2; }
	};

	// Stable slot identities let user overrides survive changes to default keys.
	struct FInputBinding
	{
		std::string Slot;
		std::string Action;
		FInputSource Source;
		FVector2d Scale{1.0, 0.0};
		bool bConsume = true;
	};

	// Context values are copied into one evaluator; equal priorities use insertion order.
	struct FInputMappingContext
	{
		std::string Name;
		int32 Priority = 0;
		bool bBlockKeyboard = false;
		bool bBlockMouse = false;
		std::vector<FInputBinding> Bindings;
	};

	// Edges are stable for one sample; a tap can start and complete in that sample.
	struct FInputActionState
	{
		FVector2d Value{0.0};
		bool bActive = false;
		bool bStarted = false;
		bool bCompleted = false;
		bool bCancelled = false;
	};

	// Immutable to consumers, with neutral reads for unknown action identities.
	class FInputActionSnapshot
	{
	public:
		ENGINE_API auto Get(std::string_view Action) const -> const FInputActionState&;
	private:
		std::map<std::string, FInputActionState, std::less<>> States;
		friend class FInputActionEvaluator;
	};

	// Owns one input session on the game thread. Mutations cancel immediately and
	// suppress held sources; evaluate once before building intent, even when paused.
	class FInputActionEvaluator
	{
	public:
		ENGINE_API auto DefineAction(std::string Name, EInputActionType Type) -> bool;
		ENGINE_API auto AddContext(FInputMappingContext Context, bool bActive = true) -> bool;
		ENGINE_API auto SetContextActive(std::string_view Name, bool bActive) -> bool;
		ENGINE_API auto RemoveContext(std::string_view Name) -> bool;
		ENGINE_API auto SetDeviceBlocked(bool bKeyboard, bool bMouse) -> void;
		ENGINE_API auto Cancel() -> void;
		ENGINE_API auto Evaluate(const FGameInputState& Input) -> const FInputActionSnapshot&;
		auto GetSnapshot() const -> const FInputActionSnapshot& { return Snapshot; }
		// A duplicate effective source in the same context is a rejected conflict.
		// Cross-context overlaps are intentional and resolved by priority.
		ENGINE_API auto Rebind(std::string_view Context, std::string_view Slot, FInputSource Source) -> FInputBindingResult;
		ENGINE_API auto ResetBindings() -> void;
		ENGINE_API auto GetBindingSource(std::string_view Context, std::string_view Slot) const -> std::optional<FInputSource>;
		ENGINE_API auto SaveOverrides(const std::filesystem::path& Path) const -> FInputBindingResult;
		// Parses and validates detached overrides before changing any live state.
		ENGINE_API auto LoadOverrides(const std::filesystem::path& Path) -> FInputBindingResult;
	private:
		struct FContext { FInputMappingContext Definition; bool bActive = true; };
		using FOverrideKey = std::pair<std::string, std::string>;
		using FOverrides = std::map<FOverrideKey, FInputSource>;
		auto ValidateOverrides(const FOverrides& Candidate) const -> FInputBindingResult;
		auto ResolveSource(const FInputMappingContext& Context, const FInputBinding& Binding, const FOverrides& Candidate) const -> FInputSource;
		std::map<std::string, EInputActionType, std::less<>> Actions;
		std::vector<FContext> Contexts;
		FOverrides Overrides;
		FInputActionSnapshot Snapshot;
		std::array<bool, 259> LastDown{};
		std::array<bool, 259> Suppressed{};
		std::map<std::string, std::array<bool, 262>, std::less<>> LastSources;
		std::set<std::string, std::less<>> PendingCancelled;
		uint64 LastFrame = 0;
		uint64 LastReset = 0;
		bool bHasSample = false;
		bool bDirty = true;
		bool bKeyboardBlocked = false;
		bool bMouseBlocked = false;
	};
}
