// Created by zhougy on 2025/10/28.

#pragma once

#include <type_traits>

namespace Doge
{
	template<typename T>
	concept Integral = std::is_integral_v<T>;
}