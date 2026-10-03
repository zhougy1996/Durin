// Created by zhougy on 2025/10/28.

#pragma once

#include "Misc/CoreStd.h"

namespace Durin
{
	template<typename T>
	concept Integral = std::is_integral_v<T>;
}