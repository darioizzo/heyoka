// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef HEYOKA_MATH_KEPE_HPP
#define HEYOKA_MATH_KEPE_HPP

#include <heyoka/config.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(HEYOKA_HAVE_REAL128)

#include <mp++/real128.hpp>

#endif

#include <heyoka/detail/fwd_decl.hpp>
#include <heyoka/detail/llvm_fwd.hpp>
#include <heyoka/detail/visibility.hpp>
#include <heyoka/func.hpp>
#include <heyoka/s11n.hpp>

namespace heyoka
{

namespace detail
{

class HEYOKA_DLL_PUBLIC kepE_impl : public func_base
{
    friend class boost::serialization::access;
    template <typename Archive>
    void serialize(Archive &ar, unsigned)
    {
        ar &boost::serialization::base_object<func_base>(*this);
    }

public:
    kepE_impl();
    explicit kepE_impl(expression, expression);

    expression diff(std::unordered_map<const void *, expression> &, const std::string &) const;
    expression diff(std::unordered_map<const void *, expression> &, const param &) const;

    [[nodiscard]] llvm::Value *llvm_eval_dbl(llvm_state &, const std::vector<llvm::Value *> &, llvm::Value *,
                                             std::uint32_t, bool) const;
    [[nodiscard]] llvm::Value *llvm_eval_ldbl(llvm_state &, const std::vector<llvm::Value *> &, llvm::Value *,
                                              std::uint32_t, bool) const;
#if defined(HEYOKA_HAVE_REAL128)
    [[nodiscard]] llvm::Value *llvm_eval_f128(llvm_state &, const std::vector<llvm::Value *> &, llvm::Value *,
                                              std::uint32_t, bool) const;
#endif

    [[nodiscard]] llvm::Function *llvm_c_eval_func_dbl(llvm_state &, std::uint32_t, bool) const;
    [[nodiscard]] llvm::Function *llvm_c_eval_func_ldbl(llvm_state &, std::uint32_t, bool) const;
#if defined(HEYOKA_HAVE_REAL128)
    [[nodiscard]] llvm::Function *llvm_c_eval_func_f128(llvm_state &, std::uint32_t, bool) const;
#endif

    taylor_dc_t::size_type taylor_decompose(taylor_dc_t &) &&;

    llvm::Value *taylor_diff_dbl(llvm_state &, const std::vector<std::uint32_t> &, const std::vector<llvm::Value *> &,
                                 llvm::Value *, llvm::Value *, std::uint32_t, std::uint32_t, std::uint32_t,
                                 std::uint32_t, bool) const;
    llvm::Value *taylor_diff_ldbl(llvm_state &, const std::vector<std::uint32_t> &, const std::vector<llvm::Value *> &,
                                  llvm::Value *, llvm::Value *, std::uint32_t, std::uint32_t, std::uint32_t,
                                  std::uint32_t, bool) const;
#if defined(HEYOKA_HAVE_REAL128)
    llvm::Value *taylor_diff_f128(llvm_state &, const std::vector<std::uint32_t> &, const std::vector<llvm::Value *> &,
                                  llvm::Value *, llvm::Value *, std::uint32_t, std::uint32_t, std::uint32_t,
                                  std::uint32_t, bool) const;
#endif

    llvm::Function *taylor_c_diff_func_dbl(llvm_state &, std::uint32_t, std::uint32_t, bool) const;
    llvm::Function *taylor_c_diff_func_ldbl(llvm_state &, std::uint32_t, std::uint32_t, bool) const;
#if defined(HEYOKA_HAVE_REAL128)
    llvm::Function *taylor_c_diff_func_f128(llvm_state &, std::uint32_t, std::uint32_t, bool) const;
#endif
};

} // namespace detail

HEYOKA_DLL_PUBLIC expression kepE(expression, expression);

HEYOKA_DLL_PUBLIC expression kepE(expression, double);
HEYOKA_DLL_PUBLIC expression kepE(expression, long double);

#if defined(HEYOKA_HAVE_REAL128)

HEYOKA_DLL_PUBLIC expression kepE(expression, mppp::real128);

#endif

HEYOKA_DLL_PUBLIC expression kepE(double, expression);
HEYOKA_DLL_PUBLIC expression kepE(long double, expression);

#if defined(HEYOKA_HAVE_REAL128)

HEYOKA_DLL_PUBLIC expression kepE(mppp::real128, expression);

#endif

} // namespace heyoka

HEYOKA_S11N_FUNC_EXPORT_KEY(heyoka::detail::kepE_impl)

#endif
