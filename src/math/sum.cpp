// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include <llvm/IR/Attributes.h>
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

#include <heyoka/detail/llvm_helpers.hpp>
#include <heyoka/detail/string_conv.hpp>
#include <heyoka/detail/type_traits.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/sum.hpp>
#include <heyoka/number.hpp>
#include <heyoka/s11n.hpp>
#include <heyoka/taylor.hpp>
#include <heyoka/variable.hpp>

namespace heyoka
{

namespace detail
{

sum_impl::sum_impl() : sum_impl(std::vector<expression>{}) {}

sum_impl::sum_impl(std::vector<expression> v) : func_base("sum", std::move(v)) {}

// NOTE: a possible improvement here is to transform
// "(x + y + -20)" into "(x + y - 20)".
// Perhaps in sum_sq() as well?
void sum_impl::to_stream(std::ostream &os) const
{
    if (args().size() == 1u) {
        // NOTE: avoid brackets if there's only 1 argument.
        os << args()[0];
    } else {
        os << '(';

        for (decltype(args().size()) i = 0; i < args().size(); ++i) {
            os << args()[i];
            if (i != args().size() - 1u) {
                os << " + ";
            }
        }

        os << ')';
    }
}

std::vector<expression> sum_impl::gradient() const
{
    return std::vector<expression>(args().size(), 1_dbl);
}

namespace
{

llvm::Value *sum_llvm_eval_impl(llvm_state &s, llvm::Type *fp_t, const func_base &fb,
                                const std::vector<llvm::Value *> &eval_arr, llvm::Value *par_ptr, llvm::Value *stride,
                                std::uint32_t batch_size, bool high_accuracy)
{
    return llvm_eval_helper(
        [&s](std::vector<llvm::Value *> args, bool) -> llvm::Value * { return pairwise_sum(s.builder(), args); }, fb, s,
        fp_t, eval_arr, par_ptr, stride, batch_size, high_accuracy);
}

} // namespace

llvm::Value *sum_impl::llvm_eval(llvm_state &s, llvm::Type *fp_t, const std::vector<llvm::Value *> &eval_arr,
                                 llvm::Value *par_ptr, llvm::Value *stride, std::uint32_t batch_size,
                                 bool high_accuracy) const
{
    return sum_llvm_eval_impl(s, fp_t, *this, eval_arr, par_ptr, stride, batch_size, high_accuracy);
}

namespace
{

[[nodiscard]] llvm::Function *sum_llvm_c_eval(llvm_state &s, llvm::Type *fp_t, const func_base &fb,
                                              std::uint32_t batch_size, bool high_accuracy)
{
    return llvm_c_eval_func_helper(
        "sum", [&s](std::vector<llvm::Value *> args, bool) { return pairwise_sum(s.builder(), args); }, fb, s, fp_t,
        batch_size, high_accuracy);
}

} // namespace

llvm::Function *sum_impl::llvm_c_eval_func(llvm_state &s, llvm::Type *fp_t, std::uint32_t batch_size,
                                           bool high_accuracy) const
{
    return sum_llvm_c_eval(s, fp_t, *this, batch_size, high_accuracy);
}

namespace
{

llvm::Value *sum_taylor_diff_impl(llvm_state &s, llvm::Type *fp_t, const sum_impl &sf,
                                  const std::vector<std::uint32_t> &deps, const std::vector<llvm::Value *> &arr,
                                  llvm::Value *par_ptr, std::uint32_t n_uvars, std::uint32_t order,
                                  std::uint32_t batch_size)
{
    // NOTE: this is prevented in the implementation
    // of the sum() function.
    assert(!sf.args().empty());

    if (!deps.empty()) {
        // LCOV_EXCL_START
        throw std::invalid_argument(fmt::format("The vector of hidden dependencies in the Taylor diff for a sum "
                                                "should be empty, but instead it has a size of {}",
                                                deps.size()));
        // LCOV_EXCL_STOP
    }

    auto &builder = s.builder();

    // Load all values to be summed in local variables and
    // do a pairwise summation.
    std::vector<llvm::Value *> vals;
    vals.reserve(static_cast<decltype(vals.size())>(sf.args().size()));
    for (const auto &arg : sf.args()) {
        std::visit(
            [&](const auto &v) {
                using type = detail::uncvref_t<decltype(v)>;

                if constexpr (std::is_same_v<type, variable>) {
                    // Variable.
                    vals.push_back(taylor_fetch_diff(arr, uname_to_index(v.name()), order, n_uvars));
                } else if constexpr (is_num_param_v<type>) {
                    // Number/param.
                    if (order == 0u) {
                        vals.push_back(taylor_codegen_numparam(s, fp_t, v, par_ptr, batch_size));
                    } else {
                        vals.push_back(vector_splat(builder, llvm_codegen(s, fp_t, number{0.}), batch_size));
                    }
                } else {
                    // LCOV_EXCL_START
                    throw std::invalid_argument("An invalid argument type was encountered while trying to build the "
                                                "Taylor derivative of a sum");
                    // LCOV_EXCL_STOP
                }
            },
            arg.value());
    }

    return pairwise_sum(builder, vals);
}

} // namespace

llvm::Value *sum_impl::taylor_diff(llvm_state &s, llvm::Type *fp_t, const std::vector<std::uint32_t> &deps,
                                   const std::vector<llvm::Value *> &arr, llvm::Value *par_ptr, llvm::Value *,
                                   std::uint32_t n_uvars, std::uint32_t order, std::uint32_t, std::uint32_t batch_size,
                                   bool) const
{
    return sum_taylor_diff_impl(s, fp_t, *this, deps, arr, par_ptr, n_uvars, order, batch_size);
}

namespace
{

llvm::Function *sum_taylor_c_diff_func_impl(llvm_state &s, llvm::Type *fp_t, const sum_impl &sf, std::uint32_t n_uvars,
                                            std::uint32_t batch_size)
{
    // NOTE: this is prevented in the implementation
    // of the sum() function.
    assert(!sf.args().empty());

    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the vector floating-point type.
    auto *val_t = make_vector_type(fp_t, batch_size);

    // Build the vector of arguments needed to determine the function name.
    std::vector<std::variant<variable, number, param>> nm_args;
    nm_args.reserve(static_cast<decltype(nm_args.size())>(sf.args().size()));
    for (const auto &arg : sf.args()) {
        nm_args.push_back(std::visit(
            [](const auto &v) -> std::variant<variable, number, param> {
                using type = detail::uncvref_t<decltype(v)>;

                if constexpr (std::is_same_v<type, func>) {
                    // LCOV_EXCL_START
                    assert(false);
                    throw;
                    // LCOV_EXCL_STOP
                } else {
                    return v;
                }
            },
            arg.value()));
    }

    // Fetch the function name and arguments.
    const auto na_pair = taylor_c_diff_func_name_args(context, fp_t, "sum", n_uvars, batch_size, nm_args);
    const auto &fname = na_pair.first;
    const auto &fargs = na_pair.second;

    // Try to see if we already created the function.
    auto f = md.getFunction(fname);

    if (f == nullptr) {
        // The function was not created before, do it now.

        // Fetch the current insertion block.
        auto *orig_bb = builder.GetInsertBlock();

        // The return type is val_t.
        auto *ft = llvm::FunctionType::get(val_t, fargs, false);
        // Create the function
        f = llvm::Function::Create(ft, llvm::Function::InternalLinkage, fname, &md);
        assert(f != nullptr);
        // NOTE: force inline.
        f->addFnAttr(llvm::Attribute::AlwaysInline);

        // Fetch the necessary function arguments.
        auto order = f->args().begin();
        auto diff_arr = f->args().begin() + 2;
        auto par_ptr = f->args().begin() + 3;
        auto terms = f->args().begin() + 5;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Load all values to be summed in local variables and
        // do a pairwise summation.
        std::vector<llvm::Value *> vals;
        vals.reserve(static_cast<decltype(vals.size())>(sf.args().size()));
        for (decltype(sf.args().size()) i = 0; i < sf.args().size(); ++i) {
            vals.push_back(std::visit(
                [&](const auto &v) -> llvm::Value * {
                    using type = detail::uncvref_t<decltype(v)>;

                    if constexpr (std::is_same_v<type, variable>) {
                        return taylor_c_load_diff(s, val_t, diff_arr, n_uvars, order, terms + i);
                    } else if constexpr (is_num_param_v<type>) {
                        // Create the return value.
                        auto retval = builder.CreateAlloca(val_t);

                        llvm_if_then_else(
                            s, builder.CreateICmpEQ(order, builder.getInt32(0)),
                            [&]() {
                                // If the order is zero, run the codegen.
                                builder.CreateStore(
                                    taylor_c_diff_numparam_codegen(s, fp_t, v, terms + i, par_ptr, batch_size), retval);
                            },
                            [&]() {
                                // Otherwise, return zero.
                                builder.CreateStore(
                                    vector_splat(builder, llvm_codegen(s, fp_t, number{0.}), batch_size), retval);
                            });

                        return builder.CreateLoad(val_t, retval);
                    } else {
                        // LCOV_EXCL_START
                        throw std::invalid_argument(
                            "An invalid argument type was encountered while trying to build the "
                            "Taylor derivative of a sum");
                        // LCOV_EXCL_STOP
                    }
                },
                sf.args()[i].value()));
        }

        builder.CreateRet(pairwise_sum(builder, vals));

        // Verify.
        s.verify_function(f);

        // Restore the original insertion block.
        builder.SetInsertPoint(orig_bb);
    } else {
        // The function was created before. Check if the signatures match.
        // NOTE: there could be a mismatch if the derivative function was created
        // and then optimised - optimisation might remove arguments which are compile-time
        // constants.
        if (!compare_function_signature(f, val_t, fargs)) {
            // LCOV_EXCL_START
            throw std::invalid_argument(
                "Inconsistent function signature for the Taylor derivative of sum() in compact mode detected");
            // LCOV_EXCL_STOP
        }
    }

    return f;
}

} // namespace

llvm::Function *sum_impl::taylor_c_diff_func(llvm_state &s, llvm::Type *fp_t, std::uint32_t n_uvars,
                                             std::uint32_t batch_size, bool) const
{
    return sum_taylor_c_diff_func_impl(s, fp_t, *this, n_uvars, batch_size);
}

} // namespace detail

expression sum(std::vector<expression> args, std::uint32_t split)
{
    if (split < 2u) {
        throw std::invalid_argument(
            fmt::format("The 'split' value for a sum must be at least 2, but it is {} instead", split));
    }

    // Partition args so that all numbers are at the end.
    const auto n_end_it = std::stable_partition(
        args.begin(), args.end(), [](const expression &ex) { return !std::holds_alternative<number>(ex.value()); });

    // If we have two or more numbers, accumulate them
    // into the first number in the second partition.
    if (n_end_it != args.end()) {
        for (auto it = n_end_it + 1; it != args.end(); ++it) {
            *n_end_it += *it;
        }

        // Remove all numbers but the first one.
        args.erase(n_end_it + 1, args.end());

        // Remove the remaining number if it is zero.
        if (is_zero(std::get<number>(n_end_it->value()))) {
            args.pop_back();
        }
    }

    if (args.empty()) {
        return 0_dbl;
    }

    // NOTE: this terminates the recursion.
    if (args.size() == 1u) {
        return std::move(args[0]);
    }

    // NOTE: ret_seq will contain a sequence
    // of sums each containing 'split' terms.
    // tmp is a temporary vector
    // used to accumulate the arguments to each
    // sum in ret_seq.
    std::vector<expression> ret_seq, tmp;
    for (auto &arg : args) {
        // LCOV_EXCL_START
#if !defined(NDEBUG)
        // NOTE: there cannot be zero numbers here because
        // the numbers were compactified earlier and
        // compactification also removes the result if zero.
        if (auto nptr = std::get_if<number>(&arg.value()); nptr && is_zero(*nptr)) {
            assert(false);
        }
#endif
        // LCOV_EXCL_STOP

        tmp.push_back(std::move(arg));
        if (tmp.size() == split) {
            // NOTE: after the move, tmp is guaranteed to be empty.
            ret_seq.emplace_back(func{detail::sum_impl{std::move(tmp)}});
            assert(tmp.empty());
        }
    }

    // NOTE: tmp is not empty if 'split' does not divide
    // exactly args.size(). In such a case, we need to do the
    // last iteration manually.
    if (!tmp.empty()) {
        // NOTE: contrary to the previous loop, here we could
        // in principle end up creating a sum_impl with only one
        // term. In such a case, for consistency with the general
        // behaviour of sum({arg}), return arg directly.
        if (tmp.size() == 1u) {
            ret_seq.emplace_back(std::move(tmp[0]));
        } else {
            ret_seq.emplace_back(func{detail::sum_impl{std::move(tmp)}});
        }
    }

    // Perform a sum over the sums.
    return sum(std::move(ret_seq));
}

} // namespace heyoka

HEYOKA_S11N_FUNC_EXPORT_IMPLEMENT(heyoka::detail::sum_impl)
