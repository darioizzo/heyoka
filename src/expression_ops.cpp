// Copyright 2020, 2021, 2022, 2023 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cassert>
#include <type_traits>
#include <utility>
#include <variant>

#include <heyoka/config.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/math/binary_op.hpp>
#include <heyoka/math/neg.hpp>
#include <heyoka/math/square.hpp>
#include <heyoka/number.hpp>
#include <heyoka/param.hpp>
#include <heyoka/variable.hpp>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#if defined(HEYOKA_HAVE_REAL)

#include <mp++/real.hpp>

#endif

HEYOKA_BEGIN_NAMESPACE

expression operator+(expression e)
{
    return e;
}

expression operator-(expression e)
{
    if (const auto *num_ptr = std::get_if<number>(&e.value())) {
        // Simplify -number to its numerical value.
        return expression{-*num_ptr};
    } else {
        if (const auto *fptr = detail::is_neg(e)) {
            // Simplify -(-x) to x.
            assert(!fptr->args().empty()); // LCOV_EXCL_LINE
            return fptr->args()[0];
        } else {
            return neg(std::move(e));
        }
    }
}

namespace detail
{

// A comparison operator intended for sorting in a canonical
// way the operands to a commutative operator/function.
// NOTE: this cannot make a set of function arguments unique, as:
// - two number arguments are considered equal to each other
//   (this could be fixed by introducing an ordering on numbers),
// - two func arguments are considered equal to each other
//   (no idea how one would implement an ordering on functions).
bool comm_ops_lt(const expression &e1, const expression &e2)
{
    return std::visit(
        [](const auto &v1, const auto &v2) {
            using type1 = uncvref_t<decltype(v1)>;
            using type2 = uncvref_t<decltype(v2)>;

            // Phase 1: handle the cases where v1 and v2
            // are the same type.

            // Both arguments are variables: use lexicographic comparison.
            if constexpr (std::is_same_v<variable, type1> && std::is_same_v<variable, type2>) {
                return v1.name() < v2.name();
            }

            // Both arguments are params: compare the indices.
            if constexpr (std::is_same_v<param, type1> && std::is_same_v<param, type2>) {
                return v1.idx() < v2.idx();
            }

            // Both arguments are numbers: equivalent.
            if constexpr (std::is_same_v<number, type1> && std::is_same_v<number, type2>) {
                return false;
            }

            // Both arguments are functions: equivalent.
            if constexpr (std::is_same_v<func, type1> && std::is_same_v<func, type2>) {
                return false;
            }

            // Phase 2: handle mixed types.

            // Number is always less than non-number.
            if constexpr (std::is_same_v<number, type1>) {
                return true;
            }

            // Function never less than non-function.
            if constexpr (std::is_same_v<func, type1>) {
                return false;
            }

            // Variable less than function, greater than anything elses.
            if constexpr (std::is_same_v<variable, type1>) {
                return std::is_same_v<type2, func>;
            }

            // Param greater than number, less than anything else.
            if constexpr (std::is_same_v<param, type1>) {
                return !std::is_same_v<type2, number>;
            }

            // LCOV_EXCL_START
            assert(false);

            return false;
            // LCOV_EXCL_STOP
        },
        e1.value(), e2.value());
}

namespace
{

// NOLINTNEXTLINE(misc-no-recursion)
expression expression_plus(const expression &e1, const expression &e2)
{
    // Simplify x + neg(y) to x - y.
    if (const auto *fptr = detail::is_neg(e2)) {
        assert(!fptr->args().empty()); // LCOV_EXCL_LINE
        return e1 - fptr->args()[0];
    }

    auto visitor = [](const auto &v1, const auto &v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, add them and return the result.
            return expression{v1 + v2};
        } else if constexpr (std::is_same_v<type1, number>) {
            // e1 number, e2 non-number.
            if (is_zero(v1)) {
                // 0 + e2 = e2.
                return expression{v2};
            }

            if constexpr (std::is_same_v<func, type2>) {
                if (const auto *pbop = v2.template extract<detail::binary_op>()) {
                    if (pbop->op() == detail::binary_op::type::add
                        && std::holds_alternative<number>(pbop->args()[0].value())) {
                        // e2 = a + x, where a is a number. Simplify e1 + (a + x) -> c + x, where c = e1 + a.
                        return expression{v1} + pbop->args()[0] + pbop->args()[1];
                    }

                    // NOTE: no need to deal with e1 + (x + a) because x + a is
                    // transformed into a + x by the addition operator.

                    if (pbop->op() == detail::binary_op::type::sub
                        && std::holds_alternative<number>(pbop->args()[0].value())) {
                        // e2 = a - x, where a is a number. Simplify e1 + (a - x) -> c - x, where c = e1 + a.
                        return expression{v1} + pbop->args()[0] - pbop->args()[1];
                    }

                    // NOTE: no need to deal with e1 + (x - a) because x - a is
                    // transformed into (-a) + x by the subtraction operator.
                }
            }

            // NOTE: fall through the standard case.
        }

        // The standard case.
        return add(expression{v1}, expression{v2});
    };

    return std::visit(visitor, e1.value(), e2.value());
}

} // namespace

} // namespace detail

// NOLINTNEXTLINE(misc-no-recursion)
expression operator+(const expression &e1, const expression &e2)
{
    if (detail::comm_ops_lt(e2, e1)) {
        return detail::expression_plus(e2, e1);
    } else {
        return detail::expression_plus(e1, e2);
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
expression operator-(const expression &e1, const expression &e2)
{
    // Simplify x - (-y) to x + y.
    if (const auto *fptr = detail::is_neg(e2)) {
        assert(!fptr->args().empty()); // LCOV_EXCL_LINE
        return e1 + fptr->args()[0];
    }

    auto visitor = [](const auto &v1, const auto &v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, subtract them.
            return expression{v1 - v2};
        } else if constexpr (std::is_same_v<type1, number>) {
            // e1 number, e2 non-number.
            if (is_zero(v1)) {
                // 0 - e2 = -e2.
                return -expression{v2};
            }

            if constexpr (std::is_same_v<func, type2>) {
                if (const auto *pbop = v2.template extract<detail::binary_op>()) {
                    if (pbop->op() == detail::binary_op::type::add
                        && std::holds_alternative<number>(pbop->args()[0].value())) {
                        // e2 = a + x, where a is a number. Simplify e1 - (a + x) -> c - x, where c = e1 - a.
                        return expression{v1} - pbop->args()[0] - pbop->args()[1];
                    }

                    // NOTE: no need to deal with e1 - (x + a) because x + a is
                    // transformed into a + x by the addition operator.

                    if (pbop->op() == detail::binary_op::type::sub
                        && std::holds_alternative<number>(pbop->args()[0].value())) {
                        // e2 = a - x, where a is a number. Simplify e1 - (a - x) -> c + x, where c = e1 - a.
                        return expression{v1} - pbop->args()[0] + pbop->args()[1];
                    }

                    // NOTE: no need to deal with e1 - (x - a) because x - a is
                    // transformed into (-a) + x by the subtraction operator.
                }
            }

            // NOTE: fall through the standard case if e1 is not zero.
        } else if constexpr (std::is_same_v<type2, number>) {
            // e1 non-number, e2 number. Turn e1 - e2 into e1 + (-e2),
            // because addition provides more simplification capabilities.
            return expression{v1} + expression{-v2};
        }

        // The standard case.
        return sub(expression{v1}, expression{v2});
    };

    return std::visit(visitor, e1.value(), e2.value());
}

namespace detail
{

namespace
{

// NOLINTNEXTLINE(misc-no-recursion)
expression expression_mul(const expression &e1, const expression &e2)
{
    const auto *fptr1 = detail::is_neg(e1);
    const auto *fptr2 = detail::is_neg(e2);

    if (fptr1 != nullptr && fptr2 != nullptr) {
        // Simplify (-x) * (-y) into x*y.
        assert(!fptr1->args().empty()); // LCOV_EXCL_LINE
        assert(!fptr2->args().empty()); // LCOV_EXCL_LINE
        return fptr1->args()[0] * fptr2->args()[0];
    }

    // Simplify x*x -> square(x) if x is not a number (otherwise,
    // we will numerically compute the result below).
    if (e1 == e2 && !std::holds_alternative<number>(e1.value())) {
        return square(e1);
    }

    auto visitor = [fptr2](const auto &v1, const auto &v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, multiply them.
            return expression{v1 * v2};
        } else if constexpr (std::is_same_v<type1, number>) {
            // e1 number, e2 non-number.
            if (is_zero(v1)) {
                // 0 * e2 = 0.
                return 0_dbl;
            }

            if (is_one(v1)) {
                // 1 * e2 = e2.
                return expression{v2};
            }

            if (is_negative_one(v1)) {
                // -1 * e2 = -e2.
                return -expression{v2};
            }

            if (fptr2 != nullptr) {
                // a * (-x) = (-a) * x.
                assert(!fptr2->args().empty()); // LCOV_EXCL_LINE
                return expression{-v1} * fptr2->args()[0];
            }

            if constexpr (std::is_same_v<func, type2>) {
                if (const auto *pbop = v2.template extract<detail::binary_op>()) {
                    if (pbop->op() == detail::binary_op::type::mul
                        && std::holds_alternative<number>(pbop->args()[0].value())) {
                        // e2 = a * x, where a is a number. Simplify e1 * (a * x) -> c * x, where c = e1 * a.
                        return expression{v1} * pbop->args()[0] * pbop->args()[1];
                    }

                    // NOTE: no need to deal with e1 * (x * a) because x * a is
                    // transformed into a * x by the multiplication operator.

                    if (pbop->op() == detail::binary_op::type::div) {
                        if (std::holds_alternative<number>(pbop->args()[0].value())) {
                            // e2 = a / x, where a is a number. Simplify e1 * (a / x) -> c / x, where c = e1 * a.
                            return expression{v1} * pbop->args()[0] / pbop->args()[1];
                        }

                        if (std::holds_alternative<number>(pbop->args()[1].value())) {
                            // e2 = x / a, where a is a number. Simplify e1 * (x / a) -> c * x, where c = e1 / a.
                            return expression{v1} / pbop->args()[1] * pbop->args()[0];
                        }
                    }
                }
            }

            // NOTE: fall through the standard case.
        }

        // The standard case.
        return mul(expression{v1}, expression{v2});
    };

    return std::visit(visitor, e1.value(), e2.value());
}

} // namespace

} // namespace detail

// NOLINTNEXTLINE(misc-no-recursion)
expression operator*(const expression &e1, const expression &e2)
{
    if (detail::comm_ops_lt(e2, e1)) {
        return detail::expression_mul(e2, e1);
    } else {
        return detail::expression_mul(e1, e2);
    }
}

// NOLINTNEXTLINE(misc-no-recursion)
expression operator/(const expression &e1, const expression &e2)
{
    const auto *fptr1 = detail::is_neg(e1);
    const auto *fptr2 = detail::is_neg(e2);

    if (fptr1 != nullptr && fptr2 != nullptr) {
        // Simplify (-x) / (-y) into x/y.
        assert(!fptr1->args().empty()); // LCOV_EXCL_LINE
        assert(!fptr2->args().empty()); // LCOV_EXCL_LINE
        return fptr1->args()[0] / fptr2->args()[0];
    }

    auto visitor = [fptr1, fptr2](const auto &v1, const auto &v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type2, number>) {
            // If the divisor is zero, always raise an error.
            if (is_zero(v2)) {
                throw zero_division_error("Division by zero");
            }
        }

        if constexpr (std::is_same_v<type1, number> && std::is_same_v<type2, number>) {
            // Both are numbers, divide them.
            return expression{v1 / v2};
        } else if constexpr (std::is_same_v<type2, number>) {
            // e1 is non-number, e2 a number.
            if (is_one(v2)) {
                // e1 / 1 = e1.
                return expression{v1};
            }

            if (is_negative_one(v2)) {
                // e1 / -1 = -e1.
                return -expression{v1};
            }

            if (fptr1 != nullptr) {
                // (-e1) / a = e1 / (-a).
                assert(!fptr1->args().empty()); // LCOV_EXCL_LINE
                return fptr1->args()[0] / expression{-v2};
            }

            if constexpr (std::is_same_v<func, type1>) {
                if (const auto *pbop = v1.template extract<detail::binary_op>()) {
                    if (pbop->op() == detail::binary_op::type::div) {
                        if (std::holds_alternative<number>(pbop->args()[0].value())) {
                            // e1 = a / x, where a is a number. Simplify (a / x) / b -> (a / b) / x.
                            return pbop->args()[0] / expression{v2} / pbop->args()[1];
                        }

                        if (std::holds_alternative<number>(pbop->args()[1].value())) {
                            // e1 = x / a, where a is a number. Simplify (x / a) / b -> x / (a * b).
                            return pbop->args()[0] / (pbop->args()[1] * expression{v2});
                        }
                    }

                    if (pbop->op() == detail::binary_op::type::mul
                        && std::holds_alternative<number>(pbop->args()[0].value())) {
                        // e1 = a * x, where a is a number. Simplify (a * x) / b -> (a / b) * x.
                        return pbop->args()[0] / expression{v2} * pbop->args()[1];
                    }

                    // NOTE: no need to handle (x * a) / b as x * a is transformed
                    // into a * x by the multiplication operator.
                }
            }

            // NOTE: fall through to the standard case.
        } else if constexpr (std::is_same_v<type1, number>) {
            // e1 is a number, e2 is non-number.
            if (is_zero(v1)) {
                // 0 / e2 == 0.
                return expression{number{0.}};
            }

            if (fptr2 != nullptr) {
                // a / (-e2) = (-a) / e2.
                assert(!fptr2->args().empty()); // LCOV_EXCL_LINE
                return expression{-v1} / fptr2->args()[0];
            }

            if constexpr (std::is_same_v<func, type2>) {
                if (const auto *pbop = v2.template extract<detail::binary_op>()) {
                    if (pbop->op() == detail::binary_op::type::div) {
                        if (std::holds_alternative<number>(pbop->args()[0].value())) {
                            // e2 = a / x, where a is a number. Simplify e1 / (a / x) -> c * x, where c = e1 / a.
                            return expression{v1} / pbop->args()[0] * pbop->args()[1];
                        }

                        if (std::holds_alternative<number>(pbop->args()[1].value())) {
                            // e2 = x / a, where a is a number. Simplify e1 / (x / a) -> c / x, where c = e1 * a.
                            return expression{v1} * pbop->args()[1] / pbop->args()[0];
                        }
                    }

                    if (pbop->op() == detail::binary_op::type::mul
                        && std::holds_alternative<number>(pbop->args()[0].value())) {
                        // e2 = a * x, where a is a number. Simplify e1 / (a * x) -> c / x, where c = e1 / a.
                        return expression{v1} / pbop->args()[0] / pbop->args()[1];
                    }

                    // NOTE: no need to handle e1 / (x * a) as x * a is transformed
                    // into a * x by the multiplication operator.
                }
            }

            // NOTE: fall through to the standard case.
        }

        // The standard case.
        return div(expression{v1}, expression{v2});
    };

    return std::visit(visitor, e1.value(), e2.value());
}

expression operator+(const expression &ex, double x)
{
    return ex + expression{x};
}

expression operator+(const expression &ex, long double x)
{
    return ex + expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator+(const expression &ex, mppp::real128 x)
{
    return ex + expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator+(const expression &ex, mppp::real x)
{
    return ex + expression{std::move(x)};
}

#endif

expression operator+(double x, const expression &ex)
{
    return expression{x} + ex;
}

expression operator+(long double x, const expression &ex)
{
    return expression{x} + ex;
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator+(mppp::real128 x, const expression &ex)
{
    return expression{x} + ex;
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator+(mppp::real x, const expression &ex)
{
    return expression{std::move(x)} + ex;
}

#endif

expression operator-(const expression &ex, double x)
{
    return ex - expression{x};
}

expression operator-(const expression &ex, long double x)
{
    return ex - expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator-(const expression &ex, mppp::real128 x)
{
    return ex - expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator-(const expression &ex, mppp::real x)
{
    return ex - expression{std::move(x)};
}

#endif

expression operator-(double x, const expression &ex)
{
    return expression{x} - ex;
}

expression operator-(long double x, const expression &ex)
{
    return expression{x} - ex;
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator-(mppp::real128 x, const expression &ex)
{
    return expression{x} - ex;
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator-(mppp::real x, const expression &ex)
{
    return expression{std::move(x)} - ex;
}

#endif

expression operator*(const expression &ex, double x)
{
    return ex *expression{x};
}

expression operator*(const expression &ex, long double x)
{
    return ex *expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator*(const expression &ex, mppp::real128 x)
{
    return ex *expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator*(const expression &ex, mppp::real x)
{
    return ex *expression{std::move(x)};
}

#endif

expression operator*(double x, const expression &ex)
{
    return expression{x} * ex;
}

expression operator*(long double x, const expression &ex)
{
    return expression{x} * ex;
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator*(mppp::real128 x, const expression &ex)
{
    return expression{x} * ex;
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator*(mppp::real x, const expression &ex)
{
    return expression{std::move(x)} * ex;
}

#endif

expression operator/(const expression &ex, double x)
{
    return ex / expression{x};
}

expression operator/(const expression &ex, long double x)
{
    return ex / expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator/(const expression &ex, mppp::real128 x)
{
    return ex / expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator/(const expression &ex, mppp::real x)
{
    return ex / expression{std::move(x)};
}

#endif

expression operator/(double x, const expression &ex)
{
    return expression{x} / ex;
}

expression operator/(long double x, const expression &ex)
{
    return expression{x} / ex;
}

#if defined(HEYOKA_HAVE_REAL128)

expression operator/(mppp::real128 x, const expression &ex)
{
    return expression{x} / ex;
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression operator/(mppp::real x, const expression &ex)
{
    return expression{std::move(x)} / ex;
}

#endif

expression &operator+=(expression &x, const expression &e)
{
    // NOTE: it is important that compound operators
    // are implemented as x = x op e, so that we properly
    // take into account arithmetic promotions for
    // numbers (and, in case of mppp::real numbers,
    // precision propagation).
    return x = x + e;
}

expression &operator-=(expression &x, const expression &e)
{
    return x = x - e;
}

expression &operator*=(expression &x, const expression &e)
{
    return x = x * e;
}

expression &operator/=(expression &x, const expression &e)
{
    return x = x / e;
}

expression &operator+=(expression &ex, double x)
{
    return ex += expression{x};
}

expression &operator+=(expression &ex, long double x)
{
    return ex += expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression &operator+=(expression &ex, mppp::real128 x)
{
    return ex += expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression &operator+=(expression &ex, mppp::real x)
{
    return ex += expression{std::move(x)};
}

#endif

expression &operator-=(expression &ex, double x)
{
    return ex -= expression{x};
}

expression &operator-=(expression &ex, long double x)
{
    return ex -= expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression &operator-=(expression &ex, mppp::real128 x)
{
    return ex -= expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression &operator-=(expression &ex, mppp::real x)
{
    return ex -= expression{std::move(x)};
}

#endif

expression &operator*=(expression &ex, double x)
{
    return ex *= expression{x};
}

expression &operator*=(expression &ex, long double x)
{
    return ex *= expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression &operator*=(expression &ex, mppp::real128 x)
{
    return ex *= expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression &operator*=(expression &ex, mppp::real x)
{
    return ex *= expression{std::move(x)};
}

#endif

expression &operator/=(expression &ex, double x)
{
    return ex /= expression{x};
}

expression &operator/=(expression &ex, long double x)
{
    return ex /= expression{x};
}

#if defined(HEYOKA_HAVE_REAL128)

expression &operator/=(expression &ex, mppp::real128 x)
{
    return ex /= expression{x};
}

#endif

#if defined(HEYOKA_HAVE_REAL)

expression &operator/=(expression &ex, mppp::real x)
{
    return ex /= expression{std::move(x)};
}

#endif

bool operator==(const expression &e1, const expression &e2)
{
    auto visitor = [](const auto &v1, const auto &v2) {
        using type1 = detail::uncvref_t<decltype(v1)>;
        using type2 = detail::uncvref_t<decltype(v2)>;

        if constexpr (std::is_same_v<type1, type2>) {
            return v1 == v2;
        } else {
            return false;
        }
    };

    return std::visit(visitor, e1.value(), e2.value());
}

bool operator!=(const expression &e1, const expression &e2)
{
    return !(e1 == e2);
}

HEYOKA_END_NAMESPACE
