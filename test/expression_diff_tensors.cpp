// Copyright 2020, 2021, 2022, 2023 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cstdint>
#include <initializer_list>
#include <random>
#include <stdexcept>
#include <vector>

#include <heyoka/expression.hpp>
#include <heyoka/llvm_state.hpp>

#include <fmt/core.h>
#include <fmt/ranges.h>

#include "catch.hpp"
#include "test_utils.hpp"

std::mt19937 rng;

using namespace heyoka;
using namespace heyoka_test;

TEST_CASE("revdiff decompose")
{
    auto [x, y] = make_vars("x", "y");

    REQUIRE(detail::revdiff_decompose({x}).first == std::vector{x, "u_0"_var});
    REQUIRE(detail::revdiff_decompose({x}).second == 1u);

    REQUIRE(detail::revdiff_decompose({par[0]}).first == std::vector{par[0], "u_0"_var});
    REQUIRE(detail::revdiff_decompose({par[0]}).second == 1u);

    REQUIRE(detail::revdiff_decompose({par[0] + x}).first == std::vector{x, par[0], "u_1"_var + "u_0"_var, "u_2"_var});
    REQUIRE(detail::revdiff_decompose({par[0] + x}).second == 2u);

    REQUIRE(detail::revdiff_decompose({(par[1] + y) * (par[0] + x)}).first
            == std::vector{x, y, par[0], par[1], ("u_2"_var + "u_0"_var), ("u_3"_var + "u_1"_var),
                           ("u_5"_var * "u_4"_var), "u_6"_var});
    REQUIRE(detail::revdiff_decompose({(par[1] + y) * (par[0] + x)}).second == 4u);

    REQUIRE(detail::revdiff_decompose({subs((par[1] + y) * (par[0] + x), {{y, 1_dbl}})}).first
            == std::vector{x, par[0], par[1], ("u_1"_var + "u_0"_var), subs("u_2"_var + y, {{y, 1_dbl}}),
                           ("u_4"_var * "u_3"_var), "u_5"_var});
    REQUIRE(detail::revdiff_decompose({subs((par[1] + y) * (par[0] + x), {{y, 1_dbl}})}).second == 3u);
}

TEST_CASE("diff_tensors basic")
{
    using Catch::Matchers::Message;

    auto [x, y] = make_vars("x", "y");

    // Let's begin with some trivial expressions.
    REQUIRE_THROWS_MATCHES(diff_tensors({1_dbl}, kw::diff_order = 0), std::invalid_argument,
                           Message("Cannot compute derivatives with respect to an empty set of arguments"));

    auto dt = diff_tensors({1_dbl}, kw::diff_order = 0, kw::diff_args = {x});
    REQUIRE(dt.get_tensors().size() == 1u);
    REQUIRE(dt.get_tensors()[0].size() == 1u);
    REQUIRE(dt.get_tensors()[0][0] == 1_dbl);
    REQUIRE(dt[{0, 0}] == 1_dbl);
    REQUIRE(dt.n_diffs() == 1u);

    REQUIRE_THROWS_MATCHES(
        (dt[{1, 0}]), std::out_of_range,
        Message(fmt::format("Cannot locate the derivative corresponding to the index vector {}", std::vector{1, 0})));
    REQUIRE_THROWS_MATCHES(
        (dt[{0, 1}]), std::out_of_range,
        Message(fmt::format("Cannot locate the derivative corresponding to the index vector {}", std::vector{0, 1})));
    REQUIRE_THROWS_MATCHES(
        (dt[{0, 2}]), std::out_of_range,
        Message(fmt::format("Cannot locate the derivative corresponding to the index vector {}", std::vector{0, 2})));

    dt = diff_tensors({1_dbl}, kw::diff_order = 1, kw::diff_args = {par[0]});
    REQUIRE(dt.get_tensors().size() == 2u);
    REQUIRE(dt.get_tensors()[0].size() == 1u);
    REQUIRE(dt.get_tensors()[0][0] == 1_dbl);
    REQUIRE(dt.get_tensors()[1].size() == 1u);
    REQUIRE(dt.get_tensors()[1][0] == 0_dbl);
    REQUIRE(dt[{0, 0}] == 1_dbl);
    REQUIRE(dt[{0, 1}] == 0_dbl);
    REQUIRE(dt.n_diffs() == 2u);

    REQUIRE_THROWS_MATCHES(
        (dt[{1, 0}]), std::out_of_range,
        Message(fmt::format("Cannot locate the derivative corresponding to the index vector {}", std::vector{1, 0})));
    REQUIRE_THROWS_MATCHES(
        (dt[{0, 2}]), std::out_of_range,
        Message(fmt::format("Cannot locate the derivative corresponding to the index vector {}", std::vector{0, 2})));

    // dt = diff_tensors({1_dbl, x}, kw::diff_order = 0, kw::diff_args = {x});
    //  auto dt = diff_tensors({x * x + y}, kw::diff_order = 2);
}

// A few tests on a default-constructed dtens instance.
TEST_CASE("dtens default")
{
    dtens dt;

    REQUIRE(dt.get_tensors().empty());
    REQUIRE(dt.n_diffs() == 0u);
}

TEST_CASE("speelpenning")
{
    fmt::print("Speelpenning's example\n");
    fmt::print("======================\n");

    std::uniform_real_distribution<double> rdist(-10., 10.);

    for (auto nvars : {3u, 10u, 20u, 50u, 100u, 200u}) {
        std::vector<double> inputs, outputs_f, outputs_r;

        std::vector<expression> vars;
        auto prod = 1_dbl;

        for (auto i = 0u; i < nvars; ++i) {
            auto cur_var = expression{fmt::format("x_{}", i)};

            vars.push_back(cur_var);
            prod *= cur_var;

            inputs.push_back(rdist(rng));
            outputs_f.push_back(0.);
            outputs_r.push_back(0.);
        }

        prod = pairwise_prod(vars);

        llvm_state s;
        // auto dc_forward = add_cfunc<double>(s, "f_forward", grad(prod, kw::diff_mode = diff_mode::forward));
        // auto dc_reverse = add_cfunc<double>(s, "f_reverse", grad(prod, kw::diff_mode = diff_mode::reverse));
        auto dc_reverse = add_cfunc<double>(s, "f_reverse", diff_tensors({prod}, kw::diff_order = 1).get_tensors()[1],
                                            kw::compact_mode = true);

        fmt::print("nvars={:<5} decomposition size={:<6}\n", nvars, dc_reverse.size() - nvars - nvars);

        s.optimise();
        s.compile();

        // auto *ff = reinterpret_cast<void (*)(double *, const double *, const double *, const double *)>(
        //     s.jit_lookup("f_forward"));

        // auto *fr = reinterpret_cast<void (*)(double *, const double *, const double *, const double *)>(
        //     s.jit_lookup("f_reverse"));

        // ff(outputs_f.data(), inputs.data(), nullptr, nullptr);
        // fr(outputs_r.data(), inputs.data(), nullptr, nullptr);

        // for (auto i = 0u; i < nvars; ++i) {
        //     REQUIRE(outputs_f[i] == approximately(outputs_r[i]));
        // }
    }
}
