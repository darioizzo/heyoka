// Copyright 2020 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef HEYOKA_LLVM_STATE_HPP
#define HEYOKA_LLVM_STATE_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <heyoka/detail/fwd_decl.hpp>
#include <heyoka/detail/visibility.hpp>

namespace heyoka
{

class HEYOKA_DLL_PUBLIC llvm_state
{
    class jit;

    std::unique_ptr<jit> m_jitter;
    std::unique_ptr<llvm::Module> m_module;
    std::unique_ptr<llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter>> m_builder;
    std::unique_ptr<llvm::legacy::FunctionPassManager> m_fpm;
    std::unique_ptr<llvm::legacy::PassManager> m_pm;
    std::unordered_map<std::string, llvm::Value *> m_named_values;
    std::unordered_map<std::string, std::pair<std::type_index, std::vector<std::type_index>>> m_sig_map;
    bool m_verify = true;
    unsigned m_opt_level;

    HEYOKA_DLL_LOCAL void check_uncompiled(const char *) const;
    HEYOKA_DLL_LOCAL void check_compiled(const char *) const;
    HEYOKA_DLL_LOCAL void check_add_name(const std::string &) const;

    template <typename T>
    HEYOKA_DLL_LOCAL void add_varargs_expression(const std::string &, const expression &,
                                                 const std::vector<std::string> &);
    HEYOKA_DLL_LOCAL void verify_function_impl(llvm::Function *);

public:
    explicit llvm_state(const std::string &, unsigned = 3);
    llvm_state(const llvm_state &) = delete;
    llvm_state(llvm_state &&) = delete;
    llvm_state &operator=(const llvm_state &) = delete;
    llvm_state &operator=(llvm_state &&) = delete;
    ~llvm_state();

    llvm::Module &module();
    llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter> &builder();
    llvm::LLVMContext &context();
    bool &verify();
    std::unordered_map<std::string, llvm::Value *> &named_values();

    const llvm::Module &module() const;
    const llvm::IRBuilder<llvm::ConstantFolder, llvm::IRBuilderDefaultInserter> &builder() const;
    const llvm::LLVMContext &context() const;
    const bool &verify() const;
    const std::unordered_map<std::string, llvm::Value *> &named_values() const;

    std::string dump() const;
    std::string dump_function(const std::string &) const;

    void verify_function(const std::string &);

    void add_dbl(const std::string &, const expression &);
    void add_ldbl(const std::string &, const expression &);

    void compile();

    std::uintptr_t jit_lookup(const std::string &);

private:
    template <typename Tup, std::size_t... S>
    static bool sig_check_args(const std::vector<std::type_index> &v, std::index_sequence<S...>)
    {
        assert(sizeof...(S) == v.size());
        static_assert(sizeof...(S) == std::tuple_size_v<Tup>);

        return ((v[S] == std::type_index(typeid(std::tuple_element_t<S, Tup>))) && ...);
    }
    // This function will check if ptr is compatible with the signature of
    // the function called "name" which was added via one of the add_*()
    // overloads.
    // NOTE: this function is supposed to be called only within
    // a fetch_*() overload, thus we don't do compiled/uncompiled
    // checks.
    template <typename Ret, typename... Args>
    auto sig_check(const std::string &name, Ret (*ptr)(Args...)) const
    {
        auto it = m_sig_map.find(name);

        if (it == m_sig_map.end()) {
            // NOTE: this could happen if jit_lookup() in fetch_*() returns a pointer
            // to some object which was not added via the add_*() overloads.
            throw std::invalid_argument("Cannot determine the signature of the function '" + name + "'");
        }

        if (it->second.first != std::type_index(typeid(Ret))) {
            throw std::invalid_argument("Function return type mismatch when trying to fetch the function '" + name
                                        + "' from the compiled module");
        }

        if (sizeof...(Args) != it->second.second.size()) {
            throw std::invalid_argument(
                "Mismatch in the number of function arguments when trying to fetch the function '" + name
                + "' from the compiled module");
        }

        // Check the types of all arguments.
        if (!sig_check_args<std::tuple<Args...>>(it->second.second, std::make_index_sequence<sizeof...(Args)>{})) {
            throw std::invalid_argument("Mismatch in the type of function arguments when trying to fetch the function '"
                                        + name + "' from the compiled module");
        }

        return ptr;
    }

    // Machinery to construct a function pointer
    // type with signature T(T, T, ..., T).
    // This type will be used in the implementation
    // of the N-ary fetch_* overloads.
    template <typename T, std::size_t>
    using always_same_t = T;

    template <typename T, std::size_t... S>
    static auto get_vararg_type_impl(std::index_sequence<S...>)
    {
        return static_cast<T (*)(always_same_t<T, S>...)>(nullptr);
    }

    template <typename T, std::size_t N>
    using vararg_f_ptr = decltype(get_vararg_type_impl<T>(std::make_index_sequence<N>{}));

public:
    template <std::size_t N>
    auto fetch_dbl(const std::string &name)
    {
        return sig_check(name, reinterpret_cast<vararg_f_ptr<double, N>>(jit_lookup(name)));
    }
    template <std::size_t N>
    auto fetch_ldbl(const std::string &name)
    {
        return sig_check(name, reinterpret_cast<vararg_f_ptr<long double, N>>(jit_lookup(name)));
    }
};

} // namespace heyoka

#endif