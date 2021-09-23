// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <sstream>

#include <heyoka/expression.hpp>
#include <heyoka/math/sigmoid.hpp>
#include <heyoka/s11n.hpp>

#include "catch.hpp"

using namespace heyoka;

TEST_CASE("sigmoid diff")
{
    auto [x, y] = make_vars("x", "y");

    REQUIRE(diff(sigmoid(x * x - y), x) == (1_dbl - sigmoid(x * x - y)) * sigmoid(x * x - y) * (2. * x));
    REQUIRE(diff(sigmoid(x * x - y), y) == -((1_dbl - sigmoid(x * x - y)) * sigmoid(x * x - y)));
}

TEST_CASE("sigmoid s11n")
{
    std::stringstream ss;

    auto [x] = make_vars("x");

    auto ex = sigmoid(x);

    {
        boost::archive::binary_oarchive oa(ss);

        oa << ex;
    }

    ex = 0_dbl;

    {
        boost::archive::binary_iarchive ia(ss);

        ia >> ex;
    }

    REQUIRE(ex == sigmoid(x));
}
