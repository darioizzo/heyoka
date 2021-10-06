// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <boost/numeric/conversion/cast.hpp>

#include <fmt/format.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/detail/fwd_decl.hpp>
#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/atan2.hpp>
#include <heyoka/math/square.hpp>
#include <heyoka/number.hpp>
#include <heyoka/s11n.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

#if defined(_MSC_VER) && !defined(__clang__)

// NOTE: MSVC has issues with the other "using"
// statement form.
using namespace fmt::literals;

#else

using fmt::literals::operator""_format;

#endif

namespace heyoka
{

namespace detail
{

atan2_impl::atan2_impl(expression y, expression x) : func_base("atan2", std::vector{std::move(y), std::move(x)}) {}

atan2_impl::atan2_impl() : atan2_impl(0_dbl, 1_dbl) {}

expression atan2_impl::diff(std::unordered_map<const void *, expression> &func_map, const std::string &s) const
{
    assert(args().size() == 2u);

    const auto &y = args()[0];
    const auto &x = args()[1];

    auto den = square(x) + square(y);

    return (x * detail::diff(func_map, y, s) - y * detail::diff(func_map, x, s)) / std::move(den);
}

expression atan2_impl::diff(std::unordered_map<const void *, expression> &func_map, const param &p) const
{
    assert(args().size() == 2u);

    const auto &y = args()[0];
    const auto &x = args()[1];

    auto den = square(x) + square(y);

    return (x * detail::diff(func_map, y, p) - y * detail::diff(func_map, x, p)) / std::move(den);
}

taylor_dc_t::size_type atan2_impl::taylor_decompose(taylor_dc_t &u_vars_defs) &&
{
    assert(args().size() == 2u);

    // Append x * x and y * y.
    u_vars_defs.emplace_back(square(args()[1]), std::vector<std::uint32_t>{});
    u_vars_defs.emplace_back(square(args()[0]), std::vector<std::uint32_t>{});

    // Append x*x + y*y.
    u_vars_defs.emplace_back(expression{"u_{}"_format(u_vars_defs.size() - 2u)}
                                 + expression{"u_{}"_format(u_vars_defs.size() - 1u)},
                             std::vector<std::uint32_t>{});

    // Append the atan2 decomposition.
    u_vars_defs.emplace_back(func{std::move(*this)}, std::vector<std::uint32_t>{});

    // Add the hidden dep.
    (u_vars_defs.end() - 1)->second.push_back(boost::numeric_cast<std::uint32_t>(u_vars_defs.size() - 2u));

    // Compute the return value (pointing to the
    // decomposed atan2).
    return u_vars_defs.size() - 1u;
}

namespace
{

// Derivative of atan2(number, number).
template <typename T, typename U, typename V,
          std::enable_if_t<std::conjunction_v<is_num_param<U>, is_num_param<V>>, int> = 0>
llvm::Value *taylor_diff_atan2_impl(llvm_state &s, const std::vector<std::uint32_t> &, const U &num0, const V &num1,
                                    const std::vector<llvm::Value *> &, llvm::Value *par_ptr, std::uint32_t,
                                    std::uint32_t order, std::uint32_t, std::uint32_t batch_size)
{
    auto &builder = s.builder();

    if (order == 0u) {
        // Do the number codegen.
        auto y = taylor_codegen_numparam<T>(s, num0, par_ptr, batch_size);
        auto x = taylor_codegen_numparam<T>(s, num1, par_ptr, batch_size);

        // Compute and return the atan2.
        return llvm_atan2(s, y, x);
    } else {
        return vector_splat(builder, codegen<T>(s, number{0.}), batch_size);
    }
}

// Derivative of atan2(var, number).
template <typename T, typename U, std::enable_if_t<is_num_param<U>::value, int> = 0>
llvm::Value *taylor_diff_atan2_impl(llvm_state &s, const std::vector<std::uint32_t> &deps, const variable &var,
                                    const U &num, const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr,
                                    std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                    std::uint32_t batch_size)
{
    assert(deps.size() == 1u); // LCOV_EXCL_LINE

    auto &builder = s.builder();

    // Fetch the index of the y variable argument.
    const auto y_idx = uname_to_index(var.name());

    // Do the codegen for the x number argument.
    auto x = taylor_codegen_numparam<T>(s, num, par_ptr, batch_size);

    if (order == 0u) {
        // Compute and return the atan2.
        return llvm_atan2(s, taylor_fetch_diff(arr, y_idx, 0, n_uvars), x);
    }

    // Splat the order.
    auto n = vector_splat(builder, codegen<T>(s, number{static_cast<T>(order)}), batch_size);

    // Compute the divisor: n * d^[0].
    const auto d_idx = deps[0];
    auto divisor = builder.CreateFMul(n, taylor_fetch_diff(arr, d_idx, 0, n_uvars));

    // Compute the first part of the dividend: n * c^[0] * b^[n].
    auto dividend = builder.CreateFMul(n, builder.CreateFMul(x, taylor_fetch_diff(arr, y_idx, order, n_uvars)));

    // Compute the second part of the dividend only for order > 1, in order to avoid
    // an empty summation.
    if (order > 1u) {
        std::vector<llvm::Value *> sum;

        // NOTE: iteration in the [1, order) range.
        for (std::uint32_t j = 1; j < order; ++j) {
            auto fac = vector_splat(builder, codegen<T>(s, number(-static_cast<T>(j))), batch_size);

            auto dnj = taylor_fetch_diff(arr, d_idx, order - j, n_uvars);
            auto aj = taylor_fetch_diff(arr, idx, j, n_uvars);

            auto tmp = builder.CreateFMul(dnj, aj);
            tmp = builder.CreateFMul(fac, tmp);
            sum.push_back(tmp);
        }

        // Update the dividend.
        dividend = builder.CreateFAdd(dividend, pairwise_sum(builder, sum));
    }

    return builder.CreateFDiv(dividend, divisor);
}

// Derivative of atan2(number, var).
template <typename T, typename U, std::enable_if_t<is_num_param<U>::value, int> = 0>
llvm::Value *taylor_diff_atan2_impl(llvm_state &s, const std::vector<std::uint32_t> &deps, const U &num,
                                    const variable &var, const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr,
                                    std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                    std::uint32_t batch_size)
{
    assert(deps.size() == 1u); // LCOV_EXCL_LINE

    auto &builder = s.builder();

    // Fetch the index of the x variable argument.
    const auto x_idx = uname_to_index(var.name());

    // Do the codegen for the y number argument.
    auto y = taylor_codegen_numparam<T>(s, num, par_ptr, batch_size);

    if (order == 0u) {
        // Compute and return the atan2.
        return llvm_atan2(s, y, taylor_fetch_diff(arr, x_idx, 0, n_uvars));
    }

    // Splat the order.
    auto n = vector_splat(builder, codegen<T>(s, number{static_cast<T>(order)}), batch_size);

    // Compute the divisor: n * d^[0].
    const auto d_idx = deps[0];
    auto divisor = builder.CreateFMul(n, taylor_fetch_diff(arr, d_idx, 0, n_uvars));

    // Compute the first part of the dividend: -n * b^[0] * c^[n].
    auto dividend = builder.CreateFMul(builder.CreateFNeg(n),
                                       builder.CreateFMul(y, taylor_fetch_diff(arr, x_idx, order, n_uvars)));

    // Compute the second part of the dividend only for order > 1, in order to avoid
    // an empty summation.
    if (order > 1u) {
        std::vector<llvm::Value *> sum;

        // NOTE: iteration in the [1, order) range.
        for (std::uint32_t j = 1; j < order; ++j) {
            auto fac = vector_splat(builder, codegen<T>(s, number(-static_cast<T>(j))), batch_size);

            auto dnj = taylor_fetch_diff(arr, d_idx, order - j, n_uvars);
            auto aj = taylor_fetch_diff(arr, idx, j, n_uvars);

            auto tmp = builder.CreateFMul(dnj, aj);
            tmp = builder.CreateFMul(fac, tmp);
            sum.push_back(tmp);
        }

        // Update the dividend.
        dividend = builder.CreateFAdd(dividend, pairwise_sum(builder, sum));
    }

    return builder.CreateFDiv(dividend, divisor);
}

// Derivative of atan2(var, var).
template <typename T>
llvm::Value *taylor_diff_atan2_impl(llvm_state &s, const std::vector<std::uint32_t> &deps, const variable &var0,
                                    const variable &var1, const std::vector<llvm::Value *> &arr, llvm::Value *,
                                    std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                    std::uint32_t batch_size)
{
    assert(deps.size() == 1u); // LCOV_EXCL_LINE

    auto &builder = s.builder();

    // Fetch the indices of the y and x variable arguments.
    const auto y_idx = uname_to_index(var0.name());
    const auto x_idx = uname_to_index(var1.name());

    if (order == 0u) {
        // Compute and return the atan2.
        return llvm_atan2(s, taylor_fetch_diff(arr, y_idx, 0, n_uvars), taylor_fetch_diff(arr, x_idx, 0, n_uvars));
    }

    // Splat the order.
    auto n = vector_splat(builder, codegen<T>(s, number{static_cast<T>(order)}), batch_size);

    // Compute the divisor: n * d^[0].
    const auto d_idx = deps[0];
    auto divisor = builder.CreateFMul(n, taylor_fetch_diff(arr, d_idx, 0, n_uvars));

    // Compute the first part of the dividend: n * (c^[0] * b^[n] - b^[0] * c^[n]).
    auto dividend
        = builder.CreateFMul(taylor_fetch_diff(arr, x_idx, 0, n_uvars), taylor_fetch_diff(arr, y_idx, order, n_uvars));
    dividend = builder.CreateFSub(dividend, builder.CreateFMul(taylor_fetch_diff(arr, y_idx, 0, n_uvars),
                                                               taylor_fetch_diff(arr, x_idx, order, n_uvars)));
    dividend = builder.CreateFMul(n, dividend);

    // Compute the second part of the dividend only for order > 1, in order to avoid
    // an empty summation.
    if (order > 1u) {
        std::vector<llvm::Value *> sum;

        // NOTE: iteration in the [1, order) range.
        for (std::uint32_t j = 1; j < order; ++j) {
            auto fac = vector_splat(builder, codegen<T>(s, number(static_cast<T>(j))), batch_size);

            auto cnj = taylor_fetch_diff(arr, x_idx, order - j, n_uvars);
            auto bj = taylor_fetch_diff(arr, y_idx, j, n_uvars);

            auto bnj = taylor_fetch_diff(arr, y_idx, order - j, n_uvars);
            auto cj = taylor_fetch_diff(arr, x_idx, j, n_uvars);

            auto dnj = taylor_fetch_diff(arr, d_idx, order - j, n_uvars);
            auto aj = taylor_fetch_diff(arr, idx, j, n_uvars);

            auto tmp1 = builder.CreateFMul(cnj, bj);
            auto tmp2 = builder.CreateFMul(bnj, cj);
            auto tmp3 = builder.CreateFMul(dnj, aj);
            auto tmp = builder.CreateFSub(builder.CreateFSub(tmp1, tmp2), tmp3);

            tmp = builder.CreateFMul(fac, tmp);
            sum.push_back(tmp);
        }

        // Update the dividend.
        dividend = builder.CreateFAdd(dividend, pairwise_sum(builder, sum));
    }

    return builder.CreateFDiv(dividend, divisor);
}

// LCOV_EXCL_START

// All the other cases.
template <typename T, typename U, typename V, typename... Args>
llvm::Value *taylor_diff_atan2_impl(llvm_state &, const std::vector<std::uint32_t> &, const U &, const V &,
                                    const std::vector<llvm::Value *> &, llvm::Value *, std::uint32_t, std::uint32_t,
                                    std::uint32_t, std::uint32_t, const Args &...)
{
    throw std::invalid_argument(
        "An invalid argument type was encountered while trying to build the Taylor derivative of atan2()");
}

// LCOV_EXCL_STOP

template <typename T>
llvm::Value *taylor_diff_atan2(llvm_state &s, const atan2_impl &f, const std::vector<std::uint32_t> &deps,
                               const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, std::uint32_t n_uvars,
                               std::uint32_t order, std::uint32_t idx, std::uint32_t batch_size)
{
    // LCOV_EXCL_START
    assert(f.args().size() == 2u);

    if (deps.size() != 1u) {
        throw std::invalid_argument("A hidden dependency vector of size 1 is expected in order to compute the Taylor "
                                    "derivative of atan2(), but a vector of size {} was passed "
                                    "instead"_format(deps.size()));
    }
    // LCOV_EXCL_STOP

    return std::visit(
        [&](const auto &v1, const auto &v2) {
            return taylor_diff_atan2_impl<T>(s, deps, v1, v2, arr, par_ptr, n_uvars, order, idx, batch_size);
        },
        f.args()[0].value(), f.args()[1].value());
}

} // namespace

llvm::Value *atan2_impl::taylor_diff_dbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                         const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                         std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                         std::uint32_t batch_size, bool) const
{
    return taylor_diff_atan2<double>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

llvm::Value *atan2_impl::taylor_diff_ldbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                          const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                          std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                          std::uint32_t batch_size, bool) const
{
    return taylor_diff_atan2<long double>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *atan2_impl::taylor_diff_f128(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                          const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                          std::uint32_t n_uvars, std::uint32_t order, std::uint32_t idx,
                                          std::uint32_t batch_size, bool) const
{
    return taylor_diff_atan2<mppp::real128>(s, *this, deps, arr, par_ptr, n_uvars, order, idx, batch_size);
}

#endif

namespace
{

// Derivative of atan2(number, number).
template <typename T, typename U, typename V,
          std::enable_if_t<std::conjunction_v<is_num_param<U>, is_num_param<V>>, int> = 0>
llvm::Function *taylor_c_diff_func_atan2_impl(llvm_state &s, const U &n0, const V &n1, std::uint32_t,
                                              std::uint32_t batch_size)
{
    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Get the function name.
    const auto fname = "heyoka_taylor_diff_atan2_{}_{}_{}"_format(
        taylor_c_diff_numparam_mangle(n0), taylor_c_diff_numparam_mangle(n1), taylor_mangle_suffix(val_t));

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr,
    // - y argument,
    // - x argument,
    // - index of d.
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::PointerType::getUnqual(val_t),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    taylor_c_diff_numparam_argtype<T>(s, n0),
                                    taylor_c_diff_numparam_argtype<T>(s, n1),
                                    llvm::Type::getInt32Ty(context)};

    // Try to see if we already created the function.
    auto f = md.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is val_t.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &md);
        assert(f != nullptr); // LCOV_EXCL_LINE

        // Fetch the necessary function arguments.
        auto ord = f->args().begin();
        auto par_ptr = f->args().begin() + 3;
        auto num_y = f->args().begin() + 5;
        auto num_x = f->args().begin() + 6;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Create the return value.
        auto retval = builder.CreateAlloca(val_t);

        llvm_if_then_else(
            s, builder.CreateICmpEQ(ord, builder.getInt32(0)),
            [&]() {
                // If the order is zero, run the codegen.
                auto ret = llvm_atan2(s, taylor_c_diff_numparam_codegen(s, n0, num_y, par_ptr, batch_size),
                                      taylor_c_diff_numparam_codegen(s, n1, num_x, par_ptr, batch_size));

                builder.CreateStore(ret, retval);
            },
            [&]() {
                // Otherwise, return zero.
                builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), retval);
            });

        // Return the result.
        builder.CreateRet(builder.CreateLoad(retval));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
        // LCOV_EXCL_START
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument(
                "Inconsistent function signature for the Taylor derivative of atan2() in compact mode detected");
        }
    }
    // LCOV_EXCL_STOP

    return f;
}

// Derivative of atan2(var, number).
template <typename T, typename U, std::enable_if_t<is_num_param<U>::value, int> = 0>
llvm::Function *taylor_c_diff_func_atan2_impl(llvm_state &s, const variable &, const U &n, std::uint32_t n_uvars,
                                              std::uint32_t batch_size)
{
    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Get the function name.
    const auto fname = "heyoka_taylor_diff_atan2_var_{}_{}_n_uvars_{}"_format(taylor_c_diff_numparam_mangle(n),
                                                                              taylor_mangle_suffix(val_t), n_uvars);

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr,
    // - idx of the y argument,
    // - x argument,
    // - index of d.
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::PointerType::getUnqual(val_t),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::Type::getInt32Ty(context),
                                    taylor_c_diff_numparam_argtype<T>(s, n),
                                    llvm::Type::getInt32Ty(context)};

    // Try to see if we already created the function.
    auto f = md.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is val_t.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &md);
        assert(f != nullptr); // LCOV_EXCL_LINE

        // Fetch the necessary function arguments.
        auto ord = f->args().begin();
        auto u_idx = f->args().begin() + 1;
        auto diff_ptr = f->args().begin() + 2;
        auto par_ptr = f->args().begin() + 3;
        auto y_idx = f->args().begin() + 5;
        auto num_x = f->args().begin() + 6;
        auto d_idx = f->args().begin() + 7;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Create the return value.
        auto retval = builder.CreateAlloca(val_t);

        // Create the accumulator.
        auto acc = builder.CreateAlloca(val_t);

        llvm_if_then_else(
            s, builder.CreateICmpEQ(ord, builder.getInt32(0)),
            [&]() {
                // For order 0, run the codegen.
                auto ret = llvm_atan2(s, taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), y_idx),
                                      taylor_c_diff_numparam_codegen(s, n, num_x, par_ptr, batch_size));

                builder.CreateStore(ret, retval);
            },
            [&]() {
                // Create FP vector version of the order.
                auto ord_v = vector_splat(builder, builder.CreateUIToFP(ord, to_llvm_type<T>(context)), batch_size);

                // Compute the divisor: ord * d^[0].
                auto divisor = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), d_idx);
                divisor = builder.CreateFMul(ord_v, divisor);

                // Init the dividend: ord * c^[0] * b^[n].
                auto dividend
                    = builder.CreateFMul(ord_v, taylor_c_diff_numparam_codegen(s, n, num_x, par_ptr, batch_size));
                dividend = builder.CreateFMul(dividend, taylor_c_load_diff(s, diff_ptr, n_uvars, ord, y_idx));

                // Init the accumulator.
                builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), acc);

                // Run the loop.
                llvm_loop_u32(s, builder.getInt32(1), ord, [&](llvm::Value *j) {
                    auto j_v = vector_splat(builder, builder.CreateUIToFP(j, to_llvm_type<T>(context)), batch_size);

                    auto d_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), d_idx);
                    auto aj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, u_idx);
                    auto tmp = builder.CreateFMul(d_nj, aj);

                    tmp = builder.CreateFMul(j_v, tmp);

                    builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(acc), tmp), acc);
                });

                // Write the result.
                builder.CreateStore(builder.CreateFDiv(builder.CreateFSub(dividend, builder.CreateLoad(acc)), divisor),
                                    retval);
            });

        // Return the result.
        builder.CreateRet(builder.CreateLoad(retval));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
        // LCOV_EXCL_START
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument(
                "Inconsistent function signatures for the Taylor derivative of atan2() in compact mode detected");
        }
    }
    // LCOV_EXCL_STOP

    return f;
}

// Derivative of atan2(number, var).
template <typename T, typename U, std::enable_if_t<is_num_param<U>::value, int> = 0>
llvm::Function *taylor_c_diff_func_atan2_impl(llvm_state &s, const U &n, const variable &, std::uint32_t n_uvars,
                                              std::uint32_t batch_size)
{
    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Get the function name.
    const auto fname = "heyoka_taylor_diff_atan2_{}_var_{}_n_uvars_{}"_format(taylor_c_diff_numparam_mangle(n),
                                                                              taylor_mangle_suffix(val_t), n_uvars);

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr,
    // - y argument,
    // - idx of the x argument,
    // - index of d.
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::PointerType::getUnqual(val_t),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    taylor_c_diff_numparam_argtype<T>(s, n),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context)};

    // Try to see if we already created the function.
    auto f = md.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is val_t.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &md);
        assert(f != nullptr); // LCOV_EXCL_LINE

        // Fetch the necessary function arguments.
        auto ord = f->args().begin();
        auto u_idx = f->args().begin() + 1;
        auto diff_ptr = f->args().begin() + 2;
        auto par_ptr = f->args().begin() + 3;
        auto num_y = f->args().begin() + 5;
        auto x_idx = f->args().begin() + 6;
        auto d_idx = f->args().begin() + 7;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Create the return value.
        auto retval = builder.CreateAlloca(val_t);

        // Create the accumulator.
        auto acc = builder.CreateAlloca(val_t);

        llvm_if_then_else(
            s, builder.CreateICmpEQ(ord, builder.getInt32(0)),
            [&]() {
                // For order 0, run the codegen.
                auto ret = llvm_atan2(s, taylor_c_diff_numparam_codegen(s, n, num_y, par_ptr, batch_size),
                                      taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), x_idx));

                builder.CreateStore(ret, retval);
            },
            [&]() {
                // Create FP vector version of the order.
                auto ord_v = vector_splat(builder, builder.CreateUIToFP(ord, to_llvm_type<T>(context)), batch_size);

                // Compute the divisor: ord * d^[0].
                auto divisor = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), d_idx);
                divisor = builder.CreateFMul(ord_v, divisor);

                // Init the dividend: -ord * b^[0] * c^[n].
                auto dividend = builder.CreateFMul(builder.CreateFNeg(ord_v),
                                                   taylor_c_diff_numparam_codegen(s, n, num_y, par_ptr, batch_size));
                dividend = builder.CreateFMul(dividend, taylor_c_load_diff(s, diff_ptr, n_uvars, ord, x_idx));

                // Init the accumulator.
                builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), acc);

                // Run the loop.
                llvm_loop_u32(s, builder.getInt32(1), ord, [&](llvm::Value *j) {
                    auto j_v = vector_splat(builder, builder.CreateUIToFP(j, to_llvm_type<T>(context)), batch_size);

                    auto d_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), d_idx);
                    auto aj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, u_idx);
                    auto tmp = builder.CreateFMul(d_nj, aj);

                    tmp = builder.CreateFMul(j_v, tmp);

                    builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(acc), tmp), acc);
                });

                // Write the result.
                builder.CreateStore(builder.CreateFDiv(builder.CreateFSub(dividend, builder.CreateLoad(acc)), divisor),
                                    retval);
            });

        // Return the result.
        builder.CreateRet(builder.CreateLoad(retval));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
        // LCOV_EXCL_START
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument(
                "Inconsistent function signatures for the Taylor derivative of atan2() in compact mode detected");
        }
    }
    // LCOV_EXCL_STOP

    return f;
}

// Derivative of atan2(var, var).
template <typename T>
llvm::Function *taylor_c_diff_func_atan2_impl(llvm_state &s, const variable &, const variable &, std::uint32_t n_uvars,
                                              std::uint32_t batch_size)
{
    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Get the function name.
    const auto fname = "heyoka_taylor_diff_atan2_var_var_{}_n_uvars_{}"_format(taylor_mangle_suffix(val_t), n_uvars);

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr,
    // - idx of the y argument,
    // - idx of the x argument,
    // - index of d.
    std::vector<llvm::Type *> fargs{llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::PointerType::getUnqual(val_t),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::PointerType::getUnqual(to_llvm_type<T>(context)),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context),
                                    llvm::Type::getInt32Ty(context)};

    // Try to see if we already created the function.
    auto f = md.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto orig_bb = builder.GetInsertBlock();

        // The return type is val_t.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &md);
        assert(f != nullptr); // LCOV_EXCL_LINE

        // Fetch the necessary function arguments.
        auto ord = f->args().begin();
        auto u_idx = f->args().begin() + 1;
        auto diff_ptr = f->args().begin() + 2;
        auto y_idx = f->args().begin() + 5;
        auto x_idx = f->args().begin() + 6;
        auto d_idx = f->args().begin() + 7;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Create the return value.
        auto retval = builder.CreateAlloca(val_t);

        // Create the accumulator.
        auto acc = builder.CreateAlloca(val_t);

        llvm_if_then_else(
            s, builder.CreateICmpEQ(ord, builder.getInt32(0)),
            [&]() {
                // For order 0, run the codegen.
                auto ret = llvm_atan2(s, taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), y_idx),
                                      taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), x_idx));

                builder.CreateStore(ret, retval);
            },
            [&]() {
                // Create FP vector version of the order.
                auto ord_v = vector_splat(builder, builder.CreateUIToFP(ord, to_llvm_type<T>(context)), batch_size);

                // Compute the divisor: ord * d^[0].
                auto divisor = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), d_idx);
                divisor = builder.CreateFMul(ord_v, divisor);

                // Init the dividend: ord * (c^[0] * b^[n] - b^[0] * c^[n]).
                auto div1 = builder.CreateFMul(taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), x_idx),
                                               taylor_c_load_diff(s, diff_ptr, n_uvars, ord, y_idx));
                auto div2 = builder.CreateFMul(taylor_c_load_diff(s, diff_ptr, n_uvars, builder.getInt32(0), y_idx),
                                               taylor_c_load_diff(s, diff_ptr, n_uvars, ord, x_idx));
                auto dividend = builder.CreateFSub(div1, div2);
                dividend = builder.CreateFMul(ord_v, dividend);

                // Init the accumulator.
                builder.CreateStore(vector_splat(builder, codegen<T>(s, number{0.}), batch_size), acc);

                // Run the loop.
                llvm_loop_u32(s, builder.getInt32(1), ord, [&](llvm::Value *j) {
                    auto j_v = vector_splat(builder, builder.CreateUIToFP(j, to_llvm_type<T>(context)), batch_size);

                    auto c_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), x_idx);
                    auto bj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, y_idx);
                    auto tmp1 = builder.CreateFMul(c_nj, bj);

                    auto b_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), y_idx);
                    auto cj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, x_idx);
                    auto tmp2 = builder.CreateFMul(b_nj, cj);

                    auto d_nj = taylor_c_load_diff(s, diff_ptr, n_uvars, builder.CreateSub(ord, j), d_idx);
                    auto aj = taylor_c_load_diff(s, diff_ptr, n_uvars, j, u_idx);
                    auto tmp3 = builder.CreateFMul(d_nj, aj);

                    auto tmp = builder.CreateFSub(builder.CreateFSub(tmp1, tmp2), tmp3);
                    tmp = builder.CreateFMul(j_v, tmp);

                    builder.CreateStore(builder.CreateFAdd(builder.CreateLoad(acc), tmp), acc);
                });

                // Write the result.
                builder.CreateStore(builder.CreateFDiv(builder.CreateFAdd(dividend, builder.CreateLoad(acc)), divisor),
                                    retval);
            });

        // Return the result.
        builder.CreateRet(builder.CreateLoad(retval));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
        // LCOV_EXCL_START
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            throw std::invalid_argument(
                "Inconsistent function signatures for the Taylor derivative of atan2() in compact mode detected");
        }
    }
    // LCOV_EXCL_STOP

    return f;
}

// LCOV_EXCL_START

// All the other cases.
template <typename T, typename U, typename V, typename... Args>
llvm::Function *taylor_c_diff_func_atan2_impl(llvm_state &, const U &, const V &, std::uint32_t, std::uint32_t,
                                              const Args &...)
{
    throw std::invalid_argument("An invalid argument type was encountered while trying to build the Taylor derivative "
                                "of atan2() in compact mode");
}

// LCOV_EXCL_STOP

template <typename T>
llvm::Function *taylor_c_diff_func_atan2(llvm_state &s, const atan2_impl &fn, std::uint32_t n_uvars,
                                         std::uint32_t batch_size)
{
    assert(fn.args().size() == 2u); // LCOV_EXCL_LINE

    return std::visit([&](const auto &v1,
                          const auto &v2) { return taylor_c_diff_func_atan2_impl<T>(s, v1, v2, n_uvars, batch_size); },
                      fn.args()[0].value(), fn.args()[1].value());
}

} // namespace

llvm::Function *atan2_impl::taylor_c_diff_func_dbl(llvm_state &s, std::uint32_t n_uvars, std::uint32_t batch_size) const
{
    return taylor_c_diff_func_atan2<double>(s, *this, n_uvars, batch_size);
}

llvm::Function *atan2_impl::taylor_c_diff_func_ldbl(llvm_state &s, std::uint32_t n_uvars,
                                                    std::uint32_t batch_size) const
{
    return taylor_c_diff_func_atan2<long double>(s, *this, n_uvars, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Function *atan2_impl::taylor_c_diff_func_f128(llvm_state &s, std::uint32_t n_uvars,
                                                    std::uint32_t batch_size) const
{
    return taylor_c_diff_func_atan2<mppp::real128>(s, *this, n_uvars, batch_size);
}

#endif

} // namespace detail

expression atan2(expression y, expression x)
{
    return expression{func{detail::atan2_impl(std::move(y), std::move(x))}};
}

expression atan2(expression y, double x)
{
    return atan2(std::move(y), expression(x));
}

expression atan2(expression y, long double x)
{
    return atan2(std::move(y), expression(x));
}

#if defined(HEYOKA_HAVE_REAL128)

expression atan2(expression y, mppp::real128 x)
{
    return atan2(std::move(y), expression(x));
}

#endif

expression atan2(double y, expression x)
{
    return atan2(expression(y), std::move(x));
}

expression atan2(long double y, expression x)
{
    return atan2(expression(y), std::move(x));
}

#if defined(HEYOKA_HAVE_REAL128)

expression atan2(mppp::real128 y, expression x)
{
    return atan2(expression(y), std::move(x));
}

#endif

} // namespace heyoka

HEYOKA_S11N_FUNC_EXPORT_IMPLEMENT(heyoka::detail::atan2_impl)
