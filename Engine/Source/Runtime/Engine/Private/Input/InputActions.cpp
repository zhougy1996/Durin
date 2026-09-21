#include "Input/InputActions.h"

#include "Input/GameInputState.h"
#include "Misc/FileIO.h"
#include "Misc/FileIO.h"
#include <iomanip>
#include <sstream>

namespace Durin
{
	auto FInputSource::IsValid() const -> bool
	{
		if (Kind == EInputSourceKind::Key)
			return (Code >= 1 && Code <= static_cast<uint16>(EKey::Delete))
				|| (Code >= 51 && Code <= 62) || (Code >= 65 && Code <= 111)
				|| (Code >= 200 && Code <= static_cast<uint16>(EKey::KeypadEnter));
		if (Kind == EInputSourceKind::MouseButton) return Code < 3;
		return Kind >= EInputSourceKind::MouseX && Kind <= EInputSourceKind::MouseWheel && Code == 0;
	}

	auto FInputActionSnapshot::Get(std::string_view Action) const -> const FInputActionState&
	{
		static const FInputActionState Empty;
		const auto It = States.find(Action);
		return It == States.end() ? Empty : It->second;
	}

	auto FInputActionEvaluator::DefineAction(std::string Name, EInputActionType Type) -> bool
	{
		if (Name.empty() || Type > EInputActionType::Axis2D || Actions.contains(Name)) return false;
		Snapshot.States.emplace(Name, FInputActionState{});
		Actions.emplace(std::move(Name), Type);
		bDirty = true;
		return true;
	}

	auto FInputActionEvaluator::AddContext(FInputMappingContext Context, bool bActive) -> bool
	{
		if (Context.Name.empty()) return false;
		for (const auto& Existing : Contexts) if (Existing.Definition.Name == Context.Name) return false;
		std::set<std::string> Slots;
		std::set<size_t> Sources;
		for (const auto& Binding : Context.Bindings)
		{
			const auto Action = Actions.find(Binding.Action);
			if (Binding.Slot.empty() || !Slots.insert(Binding.Slot).second || Action == Actions.end()
				|| !Binding.Source.IsValid() || !Sources.insert(Binding.Source.GetIndex()).second
				|| !std::isfinite(Binding.Scale.x) || !std::isfinite(Binding.Scale.y)
				|| (Binding.Scale.x == 0.0 && Binding.Scale.y == 0.0)) return false;
			if (Action->second != EInputActionType::Axis2D && Binding.Scale.y != 0.0) return false;
			if (Action->second == EInputActionType::Button
				&& (Binding.Source.GetIndex() >= 259 || Binding.Scale.x != 1.0)) return false;
		}
		Cancel();
		Contexts.push_back({std::move(Context), bActive});
		return true;
	}

	auto FInputActionEvaluator::SetContextActive(std::string_view Name, bool bActive) -> bool
	{
		for (auto& Context : Contexts)
			if (Context.Definition.Name == Name)
			{
				if (Context.bActive != bActive) { Cancel(); Context.bActive = bActive; }
				return true;
			}
		return false;
	}

	auto FInputActionEvaluator::RemoveContext(std::string_view Name) -> bool
	{
		const auto It = std::find_if(Contexts.begin(), Contexts.end(), [Name](const FContext& Context) { return Context.Definition.Name == Name; });
		if (It == Contexts.end()) return false;
		Cancel();
		Contexts.erase(It);
		std::erase_if(Overrides, [Name](const auto& Entry) { return Entry.first.first == Name; });
		return true;
	}

	auto FInputActionEvaluator::SetDeviceBlocked(bool bKeyboard, bool bMouse) -> void
	{
		if (bKeyboardBlocked == bKeyboard && bMouseBlocked == bMouse) return;
		const bool bKeyboardChanged = bKeyboardBlocked != bKeyboard;
		const bool bMouseChanged = bMouseBlocked != bMouse;
		for (auto& [Name, State] : Snapshot.States)
		{
			const auto Sources = LastSources.find(Name);
			if (Sources == LastSources.end()) continue;
			bool bAffected = false;
			for (size_t Index = 0; Index < Sources->second.size(); ++Index)
				bAffected = bAffected || (Sources->second[Index] && (Index < 256 ? bKeyboardChanged : bMouseChanged));
			if (!bAffected) continue;
			if (State.bActive) PendingCancelled.insert(Name);
			State = {};
			State.bCancelled = PendingCancelled.contains(Name);
		}
		for (size_t Index = 0; Index < LastDown.size(); ++Index)
			if (Index < 256 ? bKeyboardChanged : bMouseChanged) Suppressed[Index] = Suppressed[Index] || LastDown[Index];
		bDirty = true;
		bKeyboardBlocked = bKeyboard;
		bMouseBlocked = bMouse;
	}

	auto FInputActionEvaluator::Cancel() -> void
	{
		for (auto& [Name, State] : Snapshot.States)
		{
			if (State.bActive) PendingCancelled.insert(Name);
			State.bCancelled = PendingCancelled.contains(Name);
			State.Value = FVector2d(0.0);
			State.bActive = State.bStarted = State.bCompleted = false;
		}
		for (size_t Index = 0; Index < LastDown.size(); ++Index) Suppressed[Index] = Suppressed[Index] || LastDown[Index];
		bDirty = true;
	}

	auto FInputActionEvaluator::ResolveSource(const FInputMappingContext& Context, const FInputBinding& Binding, const FOverrides& Candidate) const -> FInputSource
	{
		const auto It = Candidate.find({Context.Name, Binding.Slot});
		return It == Candidate.end() ? Binding.Source : It->second;
	}

	auto FInputActionEvaluator::Evaluate(const FGameInputState& Input) -> const FInputActionSnapshot&
	{
		if (bHasSample && LastReset != Input.GetResetNumber())
		{
			Cancel();
			// Physical reset already rejects repeat events. A fresh press is eligible.
			Suppressed.fill(false);
			LastDown.fill(false);
		}
		if (bHasSample && LastFrame == Input.GetFrameNumber() && !bDirty) return Snapshot;
		const bool bNewFrame = !bHasSample || LastFrame != Input.GetFrameNumber();
		const bool bDiscardDelta = bHasSample && bDirty;
		for (auto& [Name, State] : Snapshot.States)
		{
			State.bStarted = State.bCompleted = false;
			State.bCancelled = PendingCancelled.contains(Name);
		}

		std::array<bool, 259> Down{};
		for (size_t Index = 0; Index < 256; ++Index) Down[Index] = Input.IsKeyDown(static_cast<EKey>(Index));
		for (size_t Index = 0; Index < 3; ++Index) Down[256 + Index] = Input.IsMouseButtonDown(static_cast<EMouseButton>(Index));
		const auto FinalDown = Down;
		if (bNewFrame)
			for (auto It = Input.GetTransitions().rbegin(); It != Input.GetTransitions().rend(); ++It) Down[It->Source] = !It->bDown;
		for (size_t Index = 0; Index < Down.size(); ++Index) if (!Down[Index]) Suppressed[Index] = false;

		std::array<bool, 262> Reserved{};
		const bool bUnavailable = !Input.IsEnabled() || !Input.IsFocused();
		for (size_t Index = 0; Index < Reserved.size(); ++Index)
			Reserved[Index] = bUnavailable || (Index < 256 ? bKeyboardBlocked || Input.IsKeyboardBlocked() : bMouseBlocked || Input.IsMouseBlocked());
		std::vector<const FContext*> Ordered;
		for (const auto& Context : Contexts) if (Context.bActive) Ordered.push_back(&Context);
		std::stable_sort(Ordered.begin(), Ordered.end(), [](const FContext* A, const FContext* B) { return A->Definition.Priority > B->Definition.Priority; });
		struct FResolvedBinding { const FInputBinding* Binding; size_t Source; };
		std::vector<FResolvedBinding> Bindings;
		std::map<std::string, std::array<bool, 262>, std::less<>> Sources;
		std::array<bool, 262> Allowed{};
		for (const auto* Context : Ordered)
		{
			auto Consumed = Reserved;
			for (const auto& Binding : Context->Definition.Bindings)
			{
				const size_t Source = ResolveSource(Context->Definition, Binding, Overrides).GetIndex();
				if (Reserved[Source]) continue;
				Bindings.push_back({&Binding, Source});
				Allowed[Source] = true;
				Sources[Binding.Action][Source] = true;
				if (Binding.bConsume) Consumed[Source] = true;
			}
			for (size_t Index = 0; Index < Consumed.size(); ++Index)
				if (Index < 256 ? Context->Definition.bBlockKeyboard : Context->Definition.bBlockMouse) Consumed[Index] = true;
			Reserved = Consumed;
		}
		for (size_t Index = 0; Index < Down.size(); ++Index)
			if (!Allowed[Index] && Down[Index]) Suppressed[Index] = true;

		auto Update = [&](bool bButtons, bool bIncludeDeltas)
		{
			std::map<std::string, FVector2d, std::less<>> Values;
			for (const auto& Resolved : Bindings)
			{
				const auto& Binding = *Resolved.Binding;
				const bool bButton = Actions.at(Binding.Action) == EInputActionType::Button;
				if (bButtons != bButton) continue;
				double Value = 0.0;
				if (Resolved.Source < Down.size()) Value = Down[Resolved.Source] && !Suppressed[Resolved.Source] ? 1.0 : 0.0;
				else if (bIncludeDeltas)
					Value = Resolved.Source == 259 ? Input.GetMouseDelta().x : Resolved.Source == 260 ? Input.GetMouseDelta().y : Input.GetMouseWheelDelta();
				if (!std::isfinite(Value)) Value = 0.0;
				auto [It, Inserted] = Values.try_emplace(Binding.Action, FVector2d(0.0));
				It->second += Binding.Scale * Value;
			}
			for (auto& [Name, State] : Snapshot.States)
			{
				if ((Actions.at(Name) == EInputActionType::Button) != bButtons) continue;
				const auto ValueIt = Values.find(Name);
				FVector2d Value = ValueIt == Values.end() ? FVector2d(0.0) : ValueIt->second;
				if (!std::isfinite(Value.x) || !std::isfinite(Value.y)) Value = FVector2d(0.0);
				const bool bActive = Value.x != 0.0 || Value.y != 0.0;
				State.bStarted = State.bStarted || (!State.bActive && bActive);
				if (State.bActive && !bActive)
				{
					// Losing routing is cancellation even when the physical key remains down.
					bool bLostSource = bUnavailable;
					const auto PreviousSources = LastSources.find(Name);
					if (PreviousSources != LastSources.end())
						for (size_t Index = 0; Index < Allowed.size(); ++Index)
							bLostSource = bLostSource || (PreviousSources->second[Index] && !Sources[Name][Index]
								&& (Index >= Down.size() || Down[Index]));
					State.bCancelled = State.bCancelled || bLostSource;
					State.bCompleted = State.bCompleted || !bLostSource;
				}
				State.bActive = bActive;
				State.Value = bButtons ? FVector2d(bActive ? 1.0 : 0.0, 0.0) : Value;
			}
		};
		Update(true, false);
		if (bNewFrame)
			for (const auto& Transition : Input.GetTransitions())
			{
				Down[Transition.Source] = Transition.bDown;
				if (!Transition.bDown) Suppressed[Transition.Source] = false;
				else if (!Allowed[Transition.Source]) Suppressed[Transition.Source] = true;
				Update(true, false);
			}
		Down = FinalDown;
		Update(false, bNewFrame && !bDiscardDelta);
		LastDown = Down;
		LastSources = std::move(Sources);
		PendingCancelled.clear();
		LastFrame = Input.GetFrameNumber();
		LastReset = Input.GetResetNumber();
		bHasSample = true;
		bDirty = false;
		return Snapshot;
	}

	auto FInputActionEvaluator::ValidateOverrides(const FOverrides& Candidate) const -> FInputBindingResult
	{
		for (const auto& [Key, Source] : Candidate)
		{
			const auto Context = std::find_if(Contexts.begin(), Contexts.end(),
				[&Key](const FContext& Entry) { return Entry.Definition.Name == Key.first; });
			if (Context == Contexts.end())
				return {EInputBindingError::UnknownContext, "Unknown input context: " + Key.first, Key.first, Key.second};
			const auto& Bindings = Context->Definition.Bindings;
			const auto Binding = std::find_if(Bindings.begin(), Bindings.end(),
				[&Key](const FInputBinding& Entry) { return Entry.Slot == Key.second; });
			if (Binding == Bindings.end())
				return {EInputBindingError::UnknownSlot, "Unknown input binding: " + Key.second, Key.first, Key.second};
			if (!Source.IsValid() || (Actions.at(Binding->Action) == EInputActionType::Button && Source.GetIndex() >= 259))
				return {EInputBindingError::InvalidSource, "Invalid source for binding " + Key.second, Key.first, Key.second};
		}
		for (const auto& Context : Contexts)
		{
			std::map<size_t, std::string> Used;
			for (const auto& Binding : Context.Definition.Bindings)
			{
				const auto [It, Inserted] = Used.emplace(ResolveSource(Context.Definition, Binding, Candidate).GetIndex(), Binding.Slot);
				if (!Inserted)
					return {EInputBindingError::BindingConflict,
						"Binding conflict in " + Context.Definition.Name + ": " + It->second + " and " + Binding.Slot,
						Context.Definition.Name, Binding.Slot, It->second};
			}
		}
		return {};
	}

	auto FInputActionEvaluator::Rebind(std::string_view Context, std::string_view Slot, FInputSource Source) -> FInputBindingResult
	{
		auto Candidate = Overrides;
		Candidate[{std::string(Context), std::string(Slot)}] = Source;
		if (auto Result = ValidateOverrides(Candidate); !Result)
		{
			if (Result.Error == EInputBindingError::BindingConflict && Result.ConflictingSlot == Slot)
				std::swap(Result.Slot, Result.ConflictingSlot);
			return Result;
		}
		Cancel();
		Overrides = std::move(Candidate);
		return {};
	}

	auto FInputActionEvaluator::ResetBindings() -> void
	{
		Cancel();
		Overrides.clear();
	}

	auto FInputActionEvaluator::GetBindingSource(std::string_view ContextName, std::string_view Slot) const -> std::optional<FInputSource>
	{
		for (const auto& Context : Contexts)
			if (Context.Definition.Name == ContextName)
				for (const auto& Binding : Context.Definition.Bindings)
					if (Binding.Slot == Slot) return ResolveSource(Context.Definition, Binding, Overrides);
		return std::nullopt;
	}

	auto FInputActionEvaluator::SaveOverrides(const std::filesystem::path& Path) const -> FInputBindingResult
	{
		std::ostringstream Stream;
		Stream << "DurinInputBindings 1\n";
		for (const auto& [Key, Source] : Overrides)
			Stream << std::quoted(Key.first) << ' ' << std::quoted(Key.second) << ' ' << static_cast<unsigned>(Source.Kind) << ' ' << Source.Code << '\n';
		const std::string Text = Stream.str();
		if (auto FileError = FFileIO::SaveArrayToFileAtomically(std::as_bytes(std::span(Text.data(), Text.size())), Path); !FileError)
			return {EInputBindingError::WriteFailed, FileError.error().ToString()};
		return {};
	}

	auto FInputActionEvaluator::LoadOverrides(const std::filesystem::path& Path) -> FInputBindingResult
	{
		std::error_code FileError;
		const auto Size = std::filesystem::file_size(Path, FileError);
		if (FileError) return {EInputBindingError::ReadFailed, "Cannot read input overrides: " + Path.string() + ": " + FileError.message()};
		if (Size > 1024 * 1024) return {EInputBindingError::InvalidFile, "Input override file exceeds 1 MiB: " + Path.string()};
		auto Text = FFileIO::LoadFileToString(Path);
		if (!Text) return {EInputBindingError::ReadFailed, Text.error().ToString()};
		if (Text->size() > 1024 * 1024) return {EInputBindingError::InvalidFile, "Input override file exceeds 1 MiB."};
		std::istringstream Stream(std::move(*Text));
		std::string Header;
		std::getline(Stream, Header);
		if (!Header.empty() && Header.back() == '\r') Header.pop_back();
		if (Header != "DurinInputBindings 1") return {EInputBindingError::UnsupportedFormat, "Unsupported input override format."};
		FOverrides Candidate;
		while (Stream >> std::ws && !Stream.eof())
		{
			std::string Context, Slot;
			unsigned Kind = 0, Code = 0;
			if (!(Stream >> std::quoted(Context) >> std::quoted(Slot) >> Kind >> Code))
				return {EInputBindingError::InvalidFile, "Malformed input override."};
			if (Kind > static_cast<unsigned>(EInputSourceKind::MouseWheel) || Code > 255)
				return {EInputBindingError::InvalidSource, "Invalid input source code.", Context, Slot};
			if (!Candidate.emplace(FOverrideKey{Context, Slot}, FInputSource{static_cast<EInputSourceKind>(Kind), static_cast<uint16>(Code)}).second)
				return {EInputBindingError::InvalidFile, "Duplicate input override.", Context, Slot};
		}
		if (const auto Result = ValidateOverrides(Candidate); !Result) return Result;
		Cancel();
		Overrides = std::move(Candidate);
		return {};
	}
}
