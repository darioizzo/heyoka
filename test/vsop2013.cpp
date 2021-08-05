// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cmath>
#include <initializer_list>
#include <vector>

#include <heyoka/celmec/vsop2013.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"

using namespace heyoka;

const std::vector dates = {2411545.0, 2415545.0, 2419545.0, 2423545.0, 2427545.0, 2431545.0,
                           2435545.0, 2439545.0, 2443545.0, 2447545.0, 2451545.0};

TEST_CASE("mercury")
{
    auto x = "x"_var;

    {
        auto merc_a = vsop2013_elliptic(1, 1, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = merc_a}, {0.}, kw::compact_mode = true};

        const std::vector values = {0.3870979635, 0.3870966235, 0.3870965607, 0.3870975307, 0.3870971271, 0.3870990120,
                                    0.3870991050, 0.3870986764, 0.3870984073, 0.3870985734, 0.3870982122};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 1e-8);
        }
    }

    {
        auto merc_lam = vsop2013_elliptic(1, 2, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = merc_lam}, {0.}, kw::compact_mode = true};

        const std::vector values = {6.2605163414, 2.9331298264, 5.8889006181, 2.5615070697, 5.5172901512, 2.1899138863,
                                    5.1457263304, 1.8183546988, 4.7741673767, 1.4467914533, 4.4026055470};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(std::sin(ta.get_state()[0]) - std::sin(values[i])) < 1e-8);
            REQUIRE(std::abs(std::cos(ta.get_state()[0]) - std::cos(values[i])) < 1e-8);
        }
    }

    {
        auto merc_k = vsop2013_elliptic(1, 3, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = merc_k}, {0.}, kw::compact_mode = true};

        const std::vector values = {0.0452614144, 0.0452099977, 0.0451485382, 0.0450934263, 0.0450275900, 0.0449601649,
                                    0.0448988996, 0.0448363569, 0.0447776649, 0.0447224543, 0.0446647836};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 1e-8);
        }
    }
}

TEST_CASE("venus")
{
    auto x = "x"_var;

    {
        auto a_sol = vsop2013_elliptic(2, 1, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = a_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {0.7233268460, 0.7233324174, 0.7233307847, 0.7233242646, 0.7233283654, 0.7233426547,
                                    0.7233248700, 0.7233262220, 0.7233314949, 0.7233310596, 0.7233269276};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 1e-8);
        }
    }

    {
        auto lam_sol = vsop2013_elliptic(2, 2, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = lam_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {3.0850544129, 1.8375355480, 0.5899962012, 5.6256196213, 4.3780843283, 3.1306248680,
                                    1.8830820759, 0.6355264420, 5.6711894725, 4.4236836479, 3.1761349270};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(std::sin(ta.get_state()[0]) - std::sin(values[i])) < 2e-8);
            REQUIRE(std::abs(std::cos(ta.get_state()[0]) - std::cos(values[i])) < 2e-8);
        }
    }

    {
        auto h_sol = vsop2013_elliptic(2, 4, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = h_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {0.0051297811, 0.0050926797, 0.0050804051, 0.0050923304, 0.0051193425, 0.0050791039,
                                    0.0050664598, 0.0050968270, 0.0050966509, 0.0050523635, 0.0050312156};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 2e-8);
        }
    }
}

TEST_CASE("emb")
{
    auto x = "x"_var;

    {
        auto a_sol = vsop2013_elliptic(3, 1, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = a_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {1.0000096358, 1.0000051435, 1.0000073760, 1.0000152008, 1.0000198307, 1.0000114829,
                                    1.0000163384, 1.0000033982, 0.9999915689, 0.9999918217, 0.9999964273};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 2e-8);
        }
    }

    {
        auto lam_sol = vsop2013_elliptic(3, 2, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = lam_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {4.8188777642, 4.5123365638, 4.2058195207, 3.8992626620, 3.5927069396, 3.2861200052,
                                    2.9795779728, 2.6730276965, 2.3664845522, 2.0599374998, 1.7534127341};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(std::sin(ta.get_state()[0]) - std::sin(values[i])) < 2e-8);
            REQUIRE(std::abs(std::cos(ta.get_state()[0]) - std::cos(values[i])) < 2e-8);
        }
    }

    {
        auto q_sol = vsop2013_elliptic(3, 5, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = q_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {0.0001248730, 0.0001127412, 0.0000987436, 0.0000866254, 0.0000744490, 0.0000620847,
                                    0.0000500081, 0.0000379132, 0.0000244774, 0.0000120455, -0.0000006055};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 2e-8);
        }
    }
}

TEST_CASE("mars")
{
    auto x = "x"_var;

    {
        auto a_sol = vsop2013_elliptic(4, 1, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = a_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {1.5236841626, 1.5236124046, 1.5236050441, 1.5236700442, 1.5236699766, 1.5236472115,
                                    1.5236402785, 1.5236249712, 1.5237113425, 1.5237954208, 1.5236789921};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 2e-8);
        }
    }

    {
        auto lam_sol = vsop2013_elliptic(4, 2, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = lam_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {4.7846953863, 3.6698641019, 2.5549157614, 1.4402987476, 0.3256326851, 5.4940145108,
                                    4.3792616969, 3.2645748509, 2.1497402787, 1.0351666881, 6.2038755297};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(std::sin(ta.get_state()[0]) - std::sin(values[i])) < 2e-8);
            REQUIRE(std::abs(std::cos(ta.get_state()[0]) - std::cos(values[i])) < 2e-8);
        }
    }

    {
        auto p_sol = vsop2013_elliptic(4, 6, kw::vsop2013_time = par[0]);
        auto ta = taylor_adaptive<double>{{prime(x) = p_sol}, {0.}, kw::compact_mode = true};

        const std::vector values = {0.0124027403, 0.0123912341, 0.0123796421, 0.0123681256, 0.0123558641, 0.0123428097,
                                    0.0123306142, 0.0123188895, 0.0123067748, 0.0122967829, 0.0122862564};

        for (auto i = 0u; i < 11u; ++i) {
            ta.set_time(0);
            ta.get_state_data()[0] = 0;

            ta.get_pars_data()[0] = (dates[i] - 2451545.0) / 365250;
            ta.propagate_until(1);

            REQUIRE(std::abs(ta.get_state()[0] - values[i]) < 2e-8);
        }
    }
}
