#pragma once

#include "Window/GenericWindow.h"

class MONA_API FGlfwWindow final : public FGenericWindow
{
public:
	virtual ~FGlfwWindow();

	static auto Make() -> TSharedPtr<FGlfwWindow>;

	virtual auto Initialize(FGenericApplication* const Application, const TSharedPtr<FGenericWindowDefinition>& Definition) -> void override;

	virtual auto PollEvents() const -> void override;

	virtual auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void override;

	virtual auto Close() -> void override;

	virtual auto ShouldClose() const -> bool override;

	auto GetOSWindowHandle() const -> void* override;

private:
	FGlfwWindow();

	EWindowMode WindowMode_ = EWindowMode::Windowed;

	FGenericApplication* OwningApplication_ = nullptr;

	GLFWwindow* GlfwWindow_ = nullptr;
};
