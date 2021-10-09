// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <heyoka/config.hpp>

#include <cassert>
#include <cstddef>
#include <ostream>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <boost/numeric/conversion/cast.hpp>

#include <fmt/format.h>
#include <fmt/ostream.h>

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
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/math/sum.hpp>
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

sum_impl::sum_impl() : sum_impl(std::vector<expression>{}) {}

sum_impl::sum_impl(std::vector<expression> v) : func_base("sum", std::move(v))
{
    for (const auto &ex : args()) {
        if (!std::holds_alternative<variable>(ex.value()) && !std::holds_alternative<func>(ex.value())) {
            throw std::invalid_argument("The 'sum()' function accepts only variables or functions as arguments, "
                                        "but the expression '{}' is neither"_format(ex));
        }
    }
}

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

llvm::Value *sum_taylor_diff_impl(llvm_state &s, const sum_impl &sf, const std::vector<std::uint32_t> &deps,
                                  const std::vector<llvm::Value *> &arr, std::uint32_t n_uvars, std::uint32_t order)
{
    // NOTE: this is prevented in the implementation
    // of the sum() function.
    assert(!sf.args().empty());

    if (!deps.empty()) {
        throw std::invalid_argument("The vector of hidden dependencies in the Taylor diff for a sum "
                                    "should be empty, but instead it has a size of {}"_format(deps.size()));
    }

    auto &builder = s.builder();

    // Load all values to be summed in local variables and
    // do a pairwise summation.
    std::vector<llvm::Value *> vals;
    vals.reserve(static_cast<decltype(vals.size())>(sf.args().size()));
    for (const auto &arg : sf.args()) {
        vals.push_back(taylor_fetch_diff(arr, uname_to_index(std::get<variable>(arg.value()).name()), order, n_uvars));
    }

    return pairwise_sum(builder, vals);
}

} // namespace

llvm::Value *sum_impl::taylor_diff_dbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                       const std::vector<llvm::Value *> &arr, llvm::Value *, llvm::Value *,
                                       std::uint32_t n_uvars, std::uint32_t order, std::uint32_t, std::uint32_t,
                                       bool) const
{
    return sum_taylor_diff_impl(s, *this, deps, arr, n_uvars, order);
}

llvm::Value *sum_impl::taylor_diff_ldbl(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                        const std::vector<llvm::Value *> &arr, llvm::Value *, llvm::Value *,
                                        std::uint32_t n_uvars, std::uint32_t order, std::uint32_t, std::uint32_t,
                                        bool) const
{
    return sum_taylor_diff_impl(s, *this, deps, arr, n_uvars, order);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Value *sum_impl::taylor_diff_f128(llvm_state &s, const std::vector<std::uint32_t> &deps,
                                        const std::vector<llvm::Value *> &arr, llvm::Value *, llvm::Value *,
                                        std::uint32_t n_uvars, std::uint32_t order, std::uint32_t, std::uint32_t,
                                        bool) const
{
    return sum_taylor_diff_impl(s, *this, deps, arr, n_uvars, order);
}

#endif

namespace
{

template <typename T>
llvm::Function *sum_taylor_c_diff_func_impl(llvm_state &s, const sum_impl &sf, std::uint32_t n_uvars,
                                            std::uint32_t batch_size)
{
    // NOTE: this is prevented in the implementation
    // of the sum() function.
    assert(!sf.args().empty());

    auto &md = s.module();
    auto &builder = s.builder();
    auto &context = s.context();

    // Fetch the floating-point type.
    auto val_t = to_llvm_vector_type<T>(context, batch_size);

    // Get the function name.
    const auto fname
        = "heyoka_taylor_diff_sum_{}_{}_n_uvars_{}"_format(taylor_mangle_suffix(val_t), sf.args().size(), n_uvars);

    // The function arguments:
    // - diff order,
    // - idx of the u variable whose diff is being computed,
    // - diff array,
    // - par ptr,
    // - time ptr,
    // - indices of the variable arguments.
    std::vector<llvm::Type *> fargs{
        llvm::Type::getInt32Ty(context), llvm::Type::getInt32Ty(context), llvm::PointerType::getUnqual(val_t),
        llvm::PointerType::getUnqual(to_llvm_type<T>(context)), llvm::PointerType::getUnqual(to_llvm_type<T>(context))};
    fargs.insert(fargs.end(), boost::numeric_cast<decltype(fargs.size())>(sf.args().size()),
                 llvm::Type::getInt32Ty(context));

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
        assert(f != nullptr);
        // NOTE: force inline.
        f->addFnAttr(llvm::Attribute::AlwaysInline);

        // Fetch the necessary function arguments.
        auto order = f->args().begin();
        auto diff_arr = f->args().begin() + 2;
        auto vars = f->args().begin() + 5;

        // Create a new basic block to start insertion into.
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", f));

        // Load all values to be summed in local variables and
        // do a pairwise summation.
        std::vector<llvm::Value *> vals;
        vals.reserve(static_cast<decltype(vals.size())>(sf.args().size()));
        for (decltype(sf.args().size()) i = 0; i < sf.args().size(); ++i) {
            vals.push_back(taylor_c_load_diff(s, diff_arr, n_uvars, order, vars + i));
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
            throw std::invalid_argument(
                "Inconsistent function signature for the Taylor derivative of sum() in compact mode detected");
        }
    }

    return f;
}

} // namespace

llvm::Function *sum_impl::taylor_c_diff_func_dbl(llvm_state &s, std::uint32_t n_uvars, std::uint32_t batch_size,
                                                 bool) const
{
    return sum_taylor_c_diff_func_impl<double>(s, *this, n_uvars, batch_size);
}

llvm::Function *sum_impl::taylor_c_diff_func_ldbl(llvm_state &s, std::uint32_t n_uvars, std::uint32_t batch_size,
                                                  bool) const
{
    return sum_taylor_c_diff_func_impl<long double>(s, *this, n_uvars, batch_size);
}

#if defined(HEYOKA_HAVE_REAL128)

llvm::Function *sum_impl::taylor_c_diff_func_f128(llvm_state &s, std::uint32_t n_uvars, std::uint32_t batch_size,
                                                  bool) const
{
    return sum_taylor_c_diff_func_impl<mppp::real128>(s, *this, n_uvars, batch_size);
}

#endif

} // namespace detail

expression sum(std::vector<expression> args, std::uint32_t split)
{
    if (split < 2u) {
        throw std::invalid_argument(
            "The 'split' value for a sum must be at least 2, but it is {} instead"_format(split));
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
        ret_seq.emplace_back(func{detail::sum_impl{std::move(tmp)}});
    }

    // Perform a sum over the sums.
    return sum(std::move(ret_seq));
}

} // namespace heyoka

HEYOKA_S11N_FUNC_EXPORT_IMPLEMENT(heyoka::detail::sum_impl)
