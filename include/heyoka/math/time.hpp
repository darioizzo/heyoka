// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef HEYOKA_MATH_TIME_HPP
#define HEYOKA_MATH_TIME_HPP

#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <heyoka/config.hpp>
#include <heyoka/detail/fwd_decl.hpp>
#include <heyoka/detail/llvm_fwd.hpp>
#include <heyoka/detail/visibility.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/s11n.hpp>

namespace heyoka
{

namespace detail
{

class HEYOKA_DLL_PUBLIC time_impl : public func_base
{
    friend class boost::serialization::access;
    template <typename Archive>
    void serialize(Archive &ar, unsigned)
    {
        ar &boost::serialization::base_object<func_base>(*this);
    }

public:
    time_impl();

    void to_stream(std::ostream &) const;

    std::vector<expression> gradient() const;

    llvm::Value *taylor_diff(llvm_state &, llvm::Type *, const std::vector<std::uint32_t> &,
                             const std::vector<llvm::Value *> &, llvm::Value *, llvm::Value *, std::uint32_t,
                             std::uint32_t, std::uint32_t, std::uint32_t, bool) const;

    llvm::Function *taylor_c_diff_func(llvm_state &, llvm::Type *, std::uint32_t, std::uint32_t, bool) const;
};

HEYOKA_DLL_PUBLIC bool is_time(const expression &);

} // namespace detail

HEYOKA_DLL_PUBLIC extern const expression time;

} // namespace heyoka

HEYOKA_S11N_FUNC_EXPORT_KEY(heyoka::detail::time_impl)

#endif
