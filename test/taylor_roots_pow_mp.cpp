// Copyright 2020, 2021, 2022, 2023, 2024 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <initializer_list>

#include <mp++/real.hpp>

#include <heyoka/kw.hpp>
#include <heyoka/math/pow.hpp>
#include <heyoka/math/sqrt.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"
#include "test_utils.hpp"

using namespace heyoka;
using namespace heyoka_test;

TEST_CASE("sqrt")
{
    using fp_t = mppp::real;

    auto x = "x"_var, y = "y"_var;

    for (auto opt_level : {0u, 3u}) {
        for (auto cm : {false, true}) {
            for (auto ha : {false, true}) {
                for (auto prec : {30, 123}) {
                    // Test with param.
                    {
                        auto ta = taylor_adaptive<fp_t>{{prime(x) = sqrt(par[0]), prime(y) = x + y},
                                                        {fp_t{2, prec}, fp_t{3, prec}},
                                                        kw::tol = 1,
                                                        kw::high_accuracy = ha,
                                                        kw::compact_mode = cm,
                                                        kw::opt_level = opt_level,
                                                        kw::pars = {fp_t(3, prec)}};

                        ta.step(true);

                        const auto jet = tc_to_jet(ta);

                        REQUIRE(jet[0] == 2);
                        REQUIRE(jet[1] == 3);
                        REQUIRE(jet[2] == approximately(sqrt(ta.get_pars()[0])));
                        REQUIRE(jet[3] == approximately(fp_t{5, prec}));
                        REQUIRE(jet[4] == 0);
                        REQUIRE(jet[5] == approximately(fp_t{1, prec} / fp_t{2, prec} * (jet[2] + jet[3])));
                    }
                    // Test with variable.
                    {
                        auto ta = taylor_adaptive<fp_t>{{prime(x) = sqrt(y + 2_dbl), prime(y) = par[0] + x},
                                                        {fp_t{2, prec}, fp_t{3, prec}},
                                                        kw::tol = 1,
                                                        kw::high_accuracy = ha,
                                                        kw::compact_mode = cm,
                                                        kw::opt_level = opt_level,
                                                        kw::pars = {fp_t(-4, prec)}};

                        ta.step(true);

                        const auto jet = tc_to_jet(ta);

                        REQUIRE(jet[0] == 2);
                        REQUIRE(jet[1] == 3);
                        REQUIRE(jet[2] == approximately(sqrt(jet[1] + fp_t(2, prec))));
                        REQUIRE(jet[3] == approximately(ta.get_pars()[0] + jet[0]));
                        REQUIRE(
                            jet[4]
                            == approximately(fp_t(.5, prec) * jet[3] / (fp_t(2, prec) * sqrt(jet[1] + fp_t(2, prec)))));
                        REQUIRE(jet[5] == approximately(fp_t(.5, prec) * jet[2]));
                    }
                }
            }
        }
    }
}

TEST_CASE("pow")
{
    using fp_t = mppp::real;

    auto x = "x"_var, y = "y"_var;

    for (auto opt_level : {0u, 3u}) {
        for (auto cm : {false, true}) {
            for (auto ha : {false, true}) {
                for (auto prec : {30, 123}) {
                    // Test with num/param.
                    {
                        auto ta = taylor_adaptive<fp_t>{{prime(x) = pow(2_dbl, par[0]), prime(y) = x + y},
                                                        {fp_t{2, prec}, fp_t{3, prec}},
                                                        kw::tol = 1,
                                                        kw::high_accuracy = ha,
                                                        kw::compact_mode = cm,
                                                        kw::opt_level = opt_level,
                                                        kw::pars = {fp_t(3, prec)}};

                        ta.step(true);

                        const auto jet = tc_to_jet(ta);

                        REQUIRE(jet[0] == 2);
                        REQUIRE(jet[1] == 3);
                        REQUIRE(jet[2] == approximately(pow(fp_t(2, prec), ta.get_pars()[0])));
                        REQUIRE(jet[3] == approximately(fp_t{5, prec}));
                        REQUIRE(jet[4] == 0);
                        REQUIRE(jet[5] == approximately(fp_t{1, prec} / fp_t{2, prec} * (jet[2] + jet[3])));
                    }

                    // Test with variable/number.
                    {
                        auto ta = taylor_adaptive<fp_t>{{prime(x) = pow(y, 2_dbl), prime(y) = pow(x, par[0])},
                                                        {fp_t{2, prec}, fp_t{3, prec}},
                                                        kw::tol = 1,
                                                        kw::high_accuracy = ha,
                                                        kw::compact_mode = cm,
                                                        kw::opt_level = opt_level,
                                                        kw::pars = {fp_t(-4, prec)}};

                        ta.step(true);

                        const auto jet = tc_to_jet(ta);

                        REQUIRE(jet[0] == 2);
                        REQUIRE(jet[1] == 3);
                        REQUIRE(jet[2] == approximately(pow(jet[1], fp_t(2, prec))));
                        REQUIRE(jet[3] == approximately(pow(jet[0], ta.get_pars()[0])));
                        REQUIRE(jet[4] == approximately(jet[3] * jet[1]));
                        REQUIRE(jet[5]
                                == approximately(fp_t(.5, prec) * ta.get_pars()[0]
                                                 * pow(jet[0], ta.get_pars()[0] - fp_t(1, prec)) * jet[2]));
                    }
                }
            }
        }
    }
}
