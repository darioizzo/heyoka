// Copyright 2020, 2021, 2022 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <sstream>

#include <heyoka/expression.hpp>
#include <heyoka/math/acos.hpp>
#include <heyoka/math/pow.hpp>
#include <heyoka/math/square.hpp>
#include <heyoka/s11n.hpp>

#include "catch.hpp"

using namespace heyoka;

TEST_CASE("acos diff var")
{
    auto [x, y] = make_vars("x", "y");

    REQUIRE(diff(acos(x * x - y), x) == -pow(1. - square(square(x) - y), -.5) * (2. * x));
    REQUIRE(diff(acos(x * x + y), y) == -pow(1. - square(square(x) + y), -.5));
}

TEST_CASE("acos diff par")
{
    auto [x, y] = make_vars("x", "y");

    REQUIRE(diff(acos(par[0] * par[0] - y), par[0]) == -pow(1. - square(square(par[0]) - y), -.5) * (2. * par[0]));
    REQUIRE(diff(acos(x * x + par[1]), par[1]) == -pow(1. - square(square(x) + par[1]), -.5));
}

TEST_CASE("acos s11n")
{
    std::stringstream ss;

    auto [x] = make_vars("x");

    auto ex = acos(x);

    {
        boost::archive::binary_oarchive oa(ss);

        oa << ex;
    }

    ex = 0_dbl;

    {
        boost::archive::binary_iarchive ia(ss);

        ia >> ex;
    }

    REQUIRE(ex == acos(x));
}
