#pragma once

#include "GimiPlan.h"
#include <cmath>

namespace gimi::detail
{
// The single float32 arithmetic definition used by both Load-time constants and
// runtime expressions. Upstream eagerly evaluates both operands of && and ||.
inline bool Unary(ExprOp op, float& value) noexcept
{
    switch (op)
    {
    case ExprOp::Positive: return true;
    case ExprOp::Negate: value = -value; return true;
    case ExprOp::Not: value = !value; return true;
    default: return false;
    }
}
inline bool Binary(ExprOp op, float& lhs, float rhs) noexcept
{
    switch (op)
    {
    case ExprOp::Add: lhs += rhs; break;
    case ExprOp::Subtract: lhs -= rhs; break;
    case ExprOp::Multiply: lhs *= rhs; break;
    case ExprOp::Divide: lhs /= rhs; break;
    case ExprOp::FloorDivide: lhs = std::floor(lhs / rhs); break;
    case ExprOp::Modulo: lhs = std::fmod(lhs, rhs); break;
    case ExprOp::Power: lhs = std::pow(lhs, rhs); break;
    case ExprOp::Equal: lhs = lhs == rhs; break;
    case ExprOp::NotEqual: lhs = lhs != rhs; break;
    case ExprOp::Less: lhs = lhs < rhs; break;
    case ExprOp::LessEqual: lhs = lhs <= rhs; break;
    case ExprOp::Greater: lhs = lhs > rhs; break;
    case ExprOp::GreaterEqual: lhs = lhs >= rhs; break;
    case ExprOp::And: lhs = lhs && rhs; break;
    case ExprOp::Or: lhs = lhs || rhs; break;
    default: return false;
    }
    return true;
}
} // namespace gimi::detail
