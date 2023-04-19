// Copyright 2020, 2021, 2022, 2023 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <llvm/IR/IRBuilder.h>

#include <heyoka/config.hpp>
#include <heyoka/detail/func_cache.hpp>
#include <heyoka/detail/llvm_fwd.hpp>
#include <heyoka/exceptions.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/func.hpp>
#include <heyoka/llvm_state.hpp>
#include <heyoka/s11n.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"

using namespace heyoka;

struct func_00 : func_base {
    func_00() : func_base("f", {}) {}
    func_00(const std::string &name) : func_base(name, {}) {}
    explicit func_00(std::vector<expression> args) : func_base("f", std::move(args)) {}
};

struct func_01 {
};

#if defined(__GNUC__)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"

#endif

TEST_CASE("func minimal")
{
    using Catch::Matchers::Message;

    func f(func_00{{"x"_var, "y"_var}});
    REQUIRE(f.get_type_index() == typeid(func_00));
    REQUIRE(f.get_name() == "f");
    REQUIRE(f.args() == std::vector{"x"_var, "y"_var});

    REQUIRE_THROWS_MATCHES(func{func_00{""}}, std::invalid_argument, Message("Cannot create a function with no name"));

    llvm_state s;

    auto *fp_t = s.builder().getDoubleTy();

    detail::funcptr_map<expression> func_map;
    REQUIRE_THROWS_MATCHES(f.diff(func_map, ""), not_implemented_error,
                           Message("Cannot compute the derivative of the function 'f' with respect to a variable, "
                                   "because the function does not provide "
                                   "neither a diff() nor a gradient() member function"));
    REQUIRE_THROWS_MATCHES(f.diff(func_map, std::get<param>(par[0].value())), not_implemented_error,
                           Message("Cannot compute the derivative of the function 'f' with respect to a parameter, "
                                   "because the function does not provide "
                                   "neither a diff() nor a gradient() member function"));
    REQUIRE_THROWS_MATCHES(f.eval_dbl({{}}, {}), not_implemented_error,
                           Message("double eval is not implemented for the function 'f'"));
    std::vector<double> tmp;
    REQUIRE_THROWS_MATCHES(f.eval_batch_dbl(tmp, {{}}, {}), not_implemented_error,
                           Message("double batch eval is not implemented for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.eval_num_dbl({1., 1.}), not_implemented_error,
                           Message("double numerical eval is not implemented for the function 'f'"));
    REQUIRE_THROWS_MATCHES(
        f.eval_num_dbl({}), std::invalid_argument,
        Message("Inconsistent number of arguments supplied to the double numerical evaluation of the function 'f': 2 "
                "arguments were expected, but 0 arguments were provided instead"));
    REQUIRE_THROWS_MATCHES(f.deval_num_dbl({1., 1.}, 0), not_implemented_error,
                           Message("double numerical eval of the derivative is not implemented for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.deval_num_dbl({1.}, 0), std::invalid_argument,
                           Message("Inconsistent number of arguments supplied to the double numerical evaluation of "
                                   "the derivative of function 'f': 2 "
                                   "arguments were expected, but 1 arguments were provided instead"));
    REQUIRE_THROWS_MATCHES(f.deval_num_dbl({1., 1.}, 2), std::invalid_argument,
                           Message("Invalid index supplied to the double numerical evaluation of the derivative of "
                                   "function 'f': index 2 was supplied, but the number of arguments is only 2"));
    REQUIRE_THROWS_MATCHES(f.llvm_eval(s, fp_t, {}, nullptr, nullptr, nullptr, 1, false), not_implemented_error,
                           Message("llvm_eval() is not implemented for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.llvm_c_eval_func(s, fp_t, 1, false), not_implemented_error,
                           Message("llvm_c_eval_func() is not implemented for the function 'f'"));

    REQUIRE(!std::is_constructible_v<func, func_01>);

    auto orig_ptr = f.get_ptr();
    REQUIRE(orig_ptr == static_cast<const func &>(f).get_ptr());

    auto f2(f);
    REQUIRE(orig_ptr == f2.get_ptr());
    REQUIRE(f2.get_type_index() == typeid(func_00));
    REQUIRE(f2.get_name() == "f");
    REQUIRE(f2.args() == std::vector{"x"_var, "y"_var});

    auto f3(std::move(f));
    REQUIRE(orig_ptr == f3.get_ptr());

    f = f3;
    REQUIRE(f.get_ptr() == f3.get_ptr());

    f = std::move(f3);
    REQUIRE(f.get_ptr() == orig_ptr);

    auto a = 0;
    auto fake_ptr = reinterpret_cast<llvm::Value *>(&a);
    REQUIRE_THROWS_MATCHES(f.taylor_diff(s, nullptr, {}, {nullptr, nullptr}, nullptr, nullptr, 2, 2, 2, 0, false),
                           std::invalid_argument,
                           Message("Null floating-point type detected in func::taylor_diff() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.taylor_diff(s, fp_t, {}, {nullptr, nullptr}, nullptr, nullptr, 2, 2, 2, 0, false),
                           std::invalid_argument,
                           Message("Null par_ptr detected in func::taylor_diff() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.taylor_diff(s, fp_t, {}, {nullptr, nullptr}, fake_ptr, nullptr, 2, 2, 2, 0, false),
                           std::invalid_argument,
                           Message("Null time_ptr detected in func::taylor_diff() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.taylor_diff(s, fp_t, {}, {nullptr, nullptr}, fake_ptr, fake_ptr, 2, 2, 2, 0, false),
                           std::invalid_argument,
                           Message("Zero batch size detected in func::taylor_diff() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.taylor_diff(s, fp_t, {}, {nullptr, nullptr}, fake_ptr, fake_ptr, 0, 2, 2, 1, false),
                           std::invalid_argument,
                           Message("Zero number of u variables detected in func::taylor_diff() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.taylor_diff(s, fp_t, {}, {nullptr, nullptr}, fake_ptr, fake_ptr, 2, 1, 2, 1, false),
                           not_implemented_error, Message("Taylor diff is not implemented for the function 'f'"));

    REQUIRE_THROWS_MATCHES(
        f.taylor_c_diff_func(s, nullptr, 2, 0, false), std::invalid_argument,
        Message("Null floating-point type detected in func::taylor_c_diff_func() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.taylor_c_diff_func(s, fp_t, 2, 0, false), std::invalid_argument,
                           Message("Zero batch size detected in func::taylor_c_diff_func() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(
        f.taylor_c_diff_func(s, fp_t, 0, 2, false), std::invalid_argument,
        Message("Zero number of u variables detected in func::taylor_c_diff_func() for the function 'f'"));
    REQUIRE_THROWS_MATCHES(f.taylor_c_diff_func(s, fp_t, 2, 1, false), not_implemented_error,
                           Message("Taylor diff in compact mode is not implemented for the function 'f'"));

    taylor_dc_t dec{{"x"_var, {}}};
    f = func{func_00{{"x"_var, "y"_var}}};
    detail::funcptr_map<taylor_dc_t::size_type> func_map2;
    f.taylor_decompose(func_map2, dec);
}

struct func_05 : func_base {
    func_05() : func_base("f", {}) {}
    explicit func_05(std::vector<expression> args) : func_base("f", std::move(args)) {}

    expression diff(detail::funcptr_map<expression> &, const std::string &) const
    {
        return 42_dbl;
    }
};

struct func_05a : func_base {
    func_05a() : func_base("f", {}) {}
    explicit func_05a(std::vector<expression> args) : func_base("f", std::move(args)) {}

    std::vector<expression> gradient() const
    {
        return {};
    }
};

struct func_05b : func_base {
    func_05b() : func_base("f", {}) {}
    explicit func_05b(std::vector<expression> args) : func_base("f", std::move(args)) {}

    expression diff(detail::funcptr_map<expression> &, const param &) const
    {
        return -42_dbl;
    }
};

TEST_CASE("func diff")
{
    using Catch::Matchers::Message;

    auto f = func(func_05{});

    detail::funcptr_map<expression> func_map;
    REQUIRE(f.diff(func_map, "x") == 42_dbl);
    REQUIRE_THROWS_MATCHES(func(func_05a{{"x"_var}}).diff(func_map, "x"), std::invalid_argument,
                           Message("Inconsistent gradient returned by the function 'f': a vector of 1 elements was "
                                   "expected, but the number of elements is 0 instead"));
    REQUIRE(func(func_05b{{"x"_var}}).diff(func_map, std::get<param>(par[0].value())) == -42_dbl);
}

struct func_06 : func_base {
    func_06() : func_base("f", {}) {}
    explicit func_06(std::vector<expression> args) : func_base("f", std::move(args)) {}

    double eval_dbl(const std::unordered_map<std::string, double> &, const std::vector<double> &) const
    {
        return 42;
    }
    long double eval_ldbl(const std::unordered_map<std::string, long double> &, const std::vector<long double> &) const
    {
        return 42;
    }
#if defined(HEYOKA_HAVE_REAL128)
    mppp::real128 eval_f128(const std::unordered_map<std::string, mppp::real128> &,
                            const std::vector<mppp::real128> &) const
    {
        return mppp::real128(42);
    }
#endif
};

TEST_CASE("func eval_dbl")
{
    auto f = func(func_06{});

    REQUIRE(f.eval_dbl({{}}, {}) == 42);
}

struct func_07 : func_base {
    func_07() : func_base("f", {}) {}
    explicit func_07(std::vector<expression> args) : func_base("f", std::move(args)) {}

    void eval_batch_dbl(std::vector<double> &, const std::unordered_map<std::string, std::vector<double>> &,
                        const std::vector<double> &) const
    {
    }
};

TEST_CASE("func eval_batch_dbl")
{
    auto f = func(func_07{});

    std::vector<double> tmp;
    REQUIRE_NOTHROW(f.eval_batch_dbl(tmp, {{}}, {}));
}

struct func_08 : func_base {
    func_08() : func_base("f", {}) {}
    explicit func_08(std::vector<expression> args) : func_base("f", std::move(args)) {}

    double eval_num_dbl(const std::vector<double> &) const
    {
        return 42;
    }
};

TEST_CASE("func eval_num_dbl")
{
    auto f = func(func_08{{"x"_var}});

    REQUIRE(f.eval_num_dbl({1.}) == 42);
}

struct func_09 : func_base {
    func_09() : func_base("f", {}) {}
    explicit func_09(std::vector<expression> args) : func_base("f", std::move(args)) {}

    double deval_num_dbl(const std::vector<double> &, std::vector<double>::size_type) const
    {
        return 43;
    }
};

TEST_CASE("func deval_num_dbl")
{
    auto f = func(func_09{{"x"_var}});

    REQUIRE(f.deval_num_dbl({1.}, 0) == 43);
}

struct func_10 : func_base {
    func_10() : func_base("f", {}) {}
    explicit func_10(std::vector<expression> args) : func_base("f", std::move(args)) {}

    taylor_dc_t::size_type taylor_decompose(taylor_dc_t &u_vars_defs) &&
    {
        u_vars_defs.emplace_back("foo", std::vector<std::uint32_t>{});

        return u_vars_defs.size() - 1u;
    }
};

struct func_10a : func_base {
    func_10a() : func_base("f", {}) {}
    explicit func_10a(std::vector<expression> args) : func_base("f", std::move(args)) {}

    taylor_dc_t::size_type taylor_decompose(taylor_dc_t &u_vars_defs) &&
    {
        u_vars_defs.emplace_back("foo", std::vector<std::uint32_t>{});

        return u_vars_defs.size();
    }
};

struct func_10b : func_base {
    func_10b() : func_base("f", {}) {}
    explicit func_10b(std::vector<expression> args) : func_base("f", std::move(args)) {}

    taylor_dc_t::size_type taylor_decompose(taylor_dc_t &u_vars_defs) &&
    {
        u_vars_defs.emplace_back("foo", std::vector<std::uint32_t>{});

        return 0;
    }
};

TEST_CASE("func taylor_decompose")
{
    using Catch::Matchers::Message;

    auto f = func(func_10{{"x"_var}});

    taylor_dc_t u_vars_defs{{"x"_var, {}}};
    detail::funcptr_map<taylor_dc_t::size_type> func_map;
    REQUIRE(f.taylor_decompose(func_map, u_vars_defs) == 1u);
    REQUIRE(u_vars_defs == taylor_dc_t{{"x"_var, {}}, {"foo"_var, {}}});

    func_map = {};

    f = func(func_10a{{"x"_var}});

    REQUIRE_THROWS_MATCHES(
        f.taylor_decompose(func_map, u_vars_defs), std::invalid_argument,
        Message("Invalid value returned by the Taylor decomposition function for the function 'f': "
                "the return value is 3, which is not less than the current size of the decomposition "
                "(3)"));

    f = func(func_10b{{"x"_var}});

    REQUIRE_THROWS_MATCHES(f.taylor_decompose(func_map, u_vars_defs), std::invalid_argument,
                           Message("The return value for the Taylor decomposition of a function can never be zero"));
}

struct func_12 : func_base {
    func_12() : func_base("f", {}) {}
    explicit func_12(std::vector<expression> args) : func_base("f", std::move(args)) {}

    llvm::Value *taylor_diff(llvm_state &, llvm::Type *, const std::vector<std::uint32_t> &,
                             const std::vector<llvm::Value *> &, llvm::Value *, llvm::Value *, std::uint32_t,
                             std::uint32_t, std::uint32_t, std::uint32_t, bool) const
    {
        return nullptr;
    }
};

TEST_CASE("func taylor diff")
{
    using Catch::Matchers::Message;

    auto f = func(func_12{});

    llvm_state s;
    auto a = 0;
    auto fake_ptr = reinterpret_cast<llvm::Value *>(&a);
    auto *fp_t = s.builder().getDoubleTy();
    REQUIRE_THROWS_MATCHES(f.taylor_diff(s, fp_t, {}, {}, fake_ptr, fake_ptr, 1, 2, 3, 4, false), std::invalid_argument,
                           Message("Null return value detected in func::taylor_diff() for the function 'f'"));
}

struct func_13 : func_base {
    func_13() : func_base("f", {}) {}
    explicit func_13(std::vector<expression> args) : func_base("f", std::move(args)) {}

    llvm::Function *taylor_c_diff_func(llvm_state &, llvm::Type *, std::uint32_t, std::uint32_t, bool) const
    {
        return nullptr;
    }
};

TEST_CASE("func taylor c_diff")
{
    using Catch::Matchers::Message;

    auto f = func(func_13{});

    llvm_state s;
    auto *fp_t = s.builder().getDoubleTy();
    REQUIRE_THROWS_MATCHES(f.taylor_c_diff_func(s, fp_t, 3, 4, false), std::invalid_argument,
                           Message("Null return value detected in func::taylor_c_diff_func() for the function 'f'"));
}

TEST_CASE("func swap")
{
    using std::swap;

    auto f1 = func(func_10{{"x"_var}});
    auto f2 = func(func_12{{"y"_var}});

    swap(f1, f2);

    REQUIRE(f1.get_type_index() == typeid(func_12));
    REQUIRE(f2.get_type_index() == typeid(func_10));
    REQUIRE(f1.args() == std::vector{"y"_var});
    REQUIRE(f2.args() == std::vector{"x"_var});

    REQUIRE(std::is_nothrow_swappable_v<func>);
}

TEST_CASE("func ostream")
{
    auto f1 = func(func_10{{"x"_var, "y"_var}});

    std::ostringstream oss;
    oss << expression{f1};

    REQUIRE(oss.str() == "f(x, y)");

    oss.str("");

    f1 = func(func_10{{"y"_var}});

    oss << expression{f1};

    REQUIRE(oss.str() == "f(y)");
}

TEST_CASE("func hash")
{
    auto f1 = func(func_10{{"x"_var, "y"_var}});

    detail::funcptr_map<std::size_t> tmp;
    REQUIRE_NOTHROW(f1.hash(tmp));

    std::cout << "Hash value for f1: " << f1.hash(tmp) << '\n';
}

struct func_14 : func_base {
    func_14(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }
    explicit func_14(std::vector<expression> args) : func_base("f", std::move(args)) {}
};

TEST_CASE("func eq ineq")
{
    auto f1 = func(func_10{{"x"_var, "y"_var}});

    detail::funcptr_map<std::size_t> tmp;

    REQUIRE(f1 == f1);
    REQUIRE(!(f1 != f1));
    REQUIRE(f1.hash(tmp) == f1.hash(tmp));

    // Differing arguments.
    auto f2 = func(func_10{{"y"_var, "x"_var}});

    REQUIRE(f1 != f2);
    REQUIRE(!(f1 == f2));

    auto f3 = func(func_14{{"x"_var, "y"_var}});
    auto f4 = func(func_14{"g", {"x"_var, "y"_var}});

    // Differing names.
    REQUIRE(f3 != f4);
    REQUIRE(!(f3 == f4));

    // Differing underlying types.
    f3 = func(func_10{{"x"_var, "y"_var}});
    f4 = func(func_14{{"x"_var, "y"_var}});

    REQUIRE(f3 != f4);
    REQUIRE(!(f3 == f4));
}

TEST_CASE("func get_variables")
{
    auto f1 = func(func_10{{}});
    REQUIRE(get_variables(expression{f1}).empty());

    f1 = func(func_10{{0_dbl}});
    REQUIRE(get_variables(expression{f1}).empty());

    f1 = func(func_10{{0_dbl, "x"_var}});
    REQUIRE(get_variables(expression{f1}) == std::vector<std::string>{"x"});

    f1 = func(func_10{{0_dbl, "y"_var, "x"_var}});
    REQUIRE(get_variables(expression{f1}) == std::vector<std::string>{"x", "y"});
    f1 = func(func_10{{0_dbl, "y"_var, "x"_var, 1_dbl, "x"_var, "y"_var, "z"_var}});
    REQUIRE(get_variables(expression{f1}) == std::vector<std::string>{"x", "y", "z"});
}

TEST_CASE("func rename_variables")
{
    auto f1 = expression{func(func_10{{}})};
    auto f2 = f1;
    f1 = rename_variables(f1, {{}});
    REQUIRE(f2 == f1);

    f1 = expression{func(func_10{{0_dbl, "x"_var}})};
    f2 = f1;
    f1 = rename_variables(f1, {{}});
    REQUIRE(f2 == f1);

    f1 = rename_variables(f1, {{"x", "y"}});
    REQUIRE(f2 != f1);
    REQUIRE(get_variables(expression{f1}) == std::vector<std::string>{"y"});
    f1 = rename_variables(f1, {{"x", "y"}});
    REQUIRE(get_variables(expression{f1}) == std::vector<std::string>{"y"});

    f1 = expression{func(func_10{{"x"_var, 0_dbl, "z"_var, "y"_var}})};
    f1 = rename_variables(f1, {{"x", "y"}});
    REQUIRE(f2 != f1);
    REQUIRE(get_variables(expression{f1}) == std::vector<std::string>{"y", "z"});
}

TEST_CASE("func diff free func")
{
    using Catch::Matchers::Message;

    auto f1 = func(func_05{{}});

    REQUIRE(diff(expression{f1}, "x") == 42_dbl);

    f1 = func(func_00{});
    REQUIRE_THROWS_MATCHES(diff(expression{f1}, ""), not_implemented_error,
                           Message("Cannot compute the derivative of the function 'f' with respect to a variable, "
                                   "because the function does not provide "
                                   "neither a diff() nor a gradient() member function"));
    REQUIRE_THROWS_MATCHES(diff(expression{f1}, par[0]), not_implemented_error,
                           Message("Cannot compute the derivative of the function 'f' with respect to a parameter, "
                                   "because the function does not provide "
                                   "neither a diff() nor a gradient() member function"));
}

struct func_15 : func_base {
    func_15(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }
    explicit func_15(std::vector<expression> args) : func_base("f", std::move(args)) {}
};

TEST_CASE("func subs")
{
    auto f1 = func(func_15{{"x"_var, "y"_var}});

    auto f2 = subs(expression{f1}, std::unordered_map<std::string, expression>{});
    REQUIRE(f2 == expression{f1});

    f2 = subs(expression{f1}, {{"x", "z"_var}});
    REQUIRE(f2 == expression{func(func_15{{"z"_var, "y"_var}})});

    f2 = subs(expression{f1}, {{"x", "z"_var}, {"y", 42_dbl}});
    REQUIRE(f2 == expression{func(func_15{{"z"_var, 42_dbl}})});
}

struct func_16 : func_base {
    func_16(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }
    explicit func_16(std::vector<expression> args) : func_base("f", std::move(args)) {}

    void to_stream(std::ostringstream &oss) const
    {
        oss << "Custom to stream";
    }
};

TEST_CASE("func to_stream")
{
    auto f1 = func(func_15{{"x"_var, "y"_var}});

    std::cout << "Default stream: " << expression{f1} << '\n';

    auto f2 = func(func_16{{"x"_var, "y"_var}});

    std::ostringstream oss;
    oss << expression{f2};
    REQUIRE(oss.str() == "Custom to stream");
}

TEST_CASE("func extract")
{
    auto f1 = func(func_15{{"x"_var, "y"_var}});

    REQUIRE(f1.extract<func_15>() != nullptr);
    REQUIRE(static_cast<const func &>(f1).extract<func_15>() != nullptr);

    REQUIRE(f1.extract<func_16>() == nullptr);
    REQUIRE(static_cast<const func &>(f1).extract<func_16>() == nullptr);

#if !defined(_MSC_VER) || defined(__clang__)
    // NOTE: vanilla MSVC does not like these extraction.
    REQUIRE(f1.extract<const func_15>() == nullptr);
    REQUIRE(static_cast<const func &>(f1).extract<const func_15>() == nullptr);

    REQUIRE(f1.extract<int>() == nullptr);
    REQUIRE(static_cast<const func &>(f1).extract<int>() == nullptr);

#endif
}

struct func_17 : func_base {
    func_17(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }
    explicit func_17(int n, std::vector<expression> args) : func_base("f", std::move(args)), value(n) {}

    bool extra_equal_to(const func &f) const
    {
        return f.extract<func_17>()->value == value;
    }

    int value = 0;
};

TEST_CASE("func extra_equal_to")
{
    auto f1 = func(func_17{0, {"x"_var, "y"_var}});
    auto f2 = func(func_17{0, {"x"_var, "y"_var}});
    auto f3 = func(func_17{1, {"x"_var, "y"_var}});

    REQUIRE(f1 == f2);
    REQUIRE(f1 != f3);
}

struct func_18 : func_base {
    func_18(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }
    explicit func_18(int n, std::vector<expression> args) : func_base("f", std::move(args)), value(n) {}

    std::size_t extra_hash() const
    {
        return static_cast<std::size_t>(value);
    }

    int value = 0;
};

TEST_CASE("func extra_hash")
{
    detail::funcptr_map<std::size_t> tmp;

    auto f1 = func(func_18{0, {"x"_var, "y"_var}});
    auto f2 = func(func_18{0, {"x"_var, "y"_var}});
    auto f3 = func(func_18{-1, {"x"_var, "y"_var}});

    REQUIRE(f1.hash(tmp) == f2.hash(tmp));
    REQUIRE(f1.hash(tmp) != f3.hash(tmp));
}

TEST_CASE("func hash eq consistency")
{
    auto [x, y, z] = make_vars("x", "y", "z");

    auto ex = x + y;

    REQUIRE(z * ex + ex == z * (x + y) + (x + y));
    REQUIRE(hash(z * ex + ex) == hash(z * (x + y) + (x + y)));
    REQUIRE(hash(z * copy(ex) + copy(ex)) == hash(z * (x + y) + (x + y)));
}

struct func_19 : func_base {
    func_19(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }

private:
    friend class boost::serialization::access;
    template <typename Archive>
    void serialize(Archive &ar, unsigned)
    {
        ar &boost::serialization::base_object<func_base>(*this);
    }
};

HEYOKA_S11N_FUNC_EXPORT(func_19)

TEST_CASE("func s11n")
{
    std::stringstream ss;

    func f{func_19{"pluto", {"x"_var}}};

    {
        boost::archive::binary_oarchive oa(ss);

        oa << f;
    }

    f = func{};

    {
        boost::archive::binary_iarchive ia(ss);

        ia >> f;
    }

    REQUIRE(f.get_name() == "pluto");
    REQUIRE(f.args().size() == 1u);
    REQUIRE(f.args()[0] == "x"_var);
}

TEST_CASE("ref semantics")
{
    auto [x, y, z] = make_vars("x", "y", "z");

    auto foo = (x + y) * z, bar = foo;

    REQUIRE(std::get<func>(foo.value()).get_ptr() == std::get<func>(bar.value()).get_ptr());

    foo = x - y;
    bar = foo;

    REQUIRE(std::get<func>(foo.value()).get_ptr() == std::get<func>(bar.value()).get_ptr());
}

TEST_CASE("copy")
{
    using Catch::Matchers::Message;

    auto [x, y, z] = make_vars("x", "y", "z");

    auto foo = ((x + y) * (z + x)) * ((z - x) * (y + x));

    // Error mode.
    REQUIRE_THROWS_MATCHES(
        expression{std::get<func>(foo.value()).copy({x, y, z})}, std::invalid_argument,
        Message("The set of new arguments passed to func::copy() has a size of 3, but the number of arguments "
                "of the original function is 2 (the two sizes must be equal)"));

    auto foo_copy = expression{std::get<func>(foo.value()).copy({x, y})};

    // Check that copy creates a new obejct.
    REQUIRE(std::get<func>(foo_copy.value()).get_ptr() != std::get<func>(foo.value()).get_ptr());

    // Check the new arguments.
    REQUIRE(std::get<func>(foo_copy.value()).args() == std::vector{x, y});
}

// Bug: a default-constructed function is not serialisable.
TEST_CASE("null func s11n")
{
    std::stringstream ss;

    func f;

    {
        boost::archive::binary_oarchive oa(ss);

        oa << f;
    }

    f = func{func_19{"pluto", {"x"_var}}};

    {
        boost::archive::binary_iarchive ia(ss);

        ia >> f;
    }

    REQUIRE(f.get_name() == "null_func");
    REQUIRE(f.args().empty());
}

struct func_20 : func_base {
    func_20(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }

    bool is_time_dependent() const
    {
        return true;
    }
};

struct func_21 : func_base {
    func_21(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }

    bool is_time_dependent()
    {
        return true;
    }
};

struct func_22 : func_base {
    func_22(std::string name = "pippo", std::vector<expression> args = {}) : func_base(std::move(name), std::move(args))
    {
    }

    int is_time_dependent() const
    {
        return 1;
    }
};

TEST_CASE("is_time_dependent")
{
    func f;
    REQUIRE(!f.is_time_dependent());

    f = func(func_20{});
    REQUIRE(f.is_time_dependent());

    f = func(func_21{});
    REQUIRE(!f.is_time_dependent());

    f = func(func_22{});
    REQUIRE(!f.is_time_dependent());
}

#if defined(__GNUC__)

#pragma GCC diagnostic pop

#endif
