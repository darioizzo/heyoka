// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef HEYOKA_MATH_SQUARE_HPP
#define HEYOKA_MATH_SQUARE_HPP

#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <heyoka/config.hpp>
#include <heyoka/detail/fwd_decl.hpp>
#include <heyoka/detail/llvm_fwd.hpp>
#include <heyoka/detail/visibility.hpp>
#include <heyoka/func.hpp>
#include <heyoka/s11n.hpp>

namespace heyoka
{

namespace detail
{

class HEYOKA_DLL_PUBLIC square_impl : public func_base
{
    friend class boost::serialization::access;
    template <typename Archive>
    void serialize(Archive &ar, unsigned)
    {
        ar &boost::serialization::base_object<func_base>(*this);
    }

public:
    square_impl();
    explicit square_impl(expression);

    void to_stream(std::ostream &) const;

    std::vector<expression> gradient() const;

    double eval_dbl(const std::unordered_map<std::string, double> &, const std::vector<double> &) const;
    long double eval_ldbl(const std::unordered_map<std::string, long double> &, const std::vector<long double> &) const;
#if defined(HEYOKA_HAVE_REAL128)
    mppp::real128 eval_f128(const std::unordered_map<std::string, mppp::real128> &,
                            const std::vector<mppp::real128> &) const;
#endif

    [[nodiscard]] llvm::Value *llvm_eval_dbl(llvm_state &, const std::vector<llvm::Value *> &, llvm::Value *,
                                             llvm::Value *, std::uint32_t, bool) const;
    [[nodiscard]] llvm::Value *llvm_eval_ldbl(llvm_state &, const std::vector<llvm::Value *> &, llvm::Value *,
                                              llvm::Value *, std::uint32_t, bool) const;
#if defined(HEYOKA_HAVE_REAL128)
    [[nodiscard]] llvm::Value *llvm_eval_f128(llvm_state &, const std::vector<llvm::Value *> &, llvm::Value *,
                                              llvm::Value *, std::uint32_t, bool) const;
#endif

    [[nodiscard]] llvm::Function *llvm_c_eval_func_dbl(llvm_state &, std::uint32_t, bool) const;
    [[nodiscard]] llvm::Function *llvm_c_eval_func_ldbl(llvm_state &, std::uint32_t, bool) const;
#if defined(HEYOKA_HAVE_REAL128)
    [[nodiscard]] llvm::Function *llvm_c_eval_func_f128(llvm_state &, std::uint32_t, bool) const;
#endif

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

HEYOKA_DLL_PUBLIC expression square(expression);

} // namespace heyoka

HEYOKA_S11N_FUNC_EXPORT_KEY(heyoka::detail::square_impl)

#endif
