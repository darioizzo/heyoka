// Copyright 2020, 2021, 2022, 2023 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <functional>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <heyoka/expression.hpp>
#include <heyoka/model/pendulum.hpp>
#include <heyoka/s11n.hpp>
#include <heyoka/step_callback.hpp>
#include <heyoka/taylor.hpp>

#include "catch.hpp"

using namespace heyoka;

template <typename TA>
bool cb0(TA &)
{
    return true;
}

struct cb1 {
    template <typename TA>
    bool operator()(TA &ta)
    {
        ta.get_state_data()[0] = 2;
        return false;
    }

    template <typename TA>
    void pre_hook(TA &ta)
    {
        ta.get_state_data()[0] = 1;
    }
};

TEST_CASE("step_callback basics")
{
    taylor_adaptive<double> ta;

    {
        step_callback<double> step_cb;

        REQUIRE(!step_cb);

        REQUIRE_THROWS_AS(step_cb(ta), std::bad_function_call);
        REQUIRE_THROWS_AS(step_cb.pre_hook(ta), std::bad_function_call);

        REQUIRE(std::is_nothrow_swappable_v<step_callback<double>>);

        REQUIRE(!std::is_constructible_v<step_callback<double>, void>);
        REQUIRE(!std::is_constructible_v<step_callback<double>, int, int>);

#if !defined(_MSC_VER) || defined(__clang__)

        // NOTE: vanilla MSVC does not like these extraction.
        REQUIRE(step_cb.extract<int>() == nullptr);
        REQUIRE(std::as_const(step_cb).extract<int>() == nullptr);
#endif

        REQUIRE(step_cb.get_type_index() == typeid(void));

        // Copy construction of empty callback.
        auto step_cb2 = step_cb;
        REQUIRE(!step_cb2);

        // Move construction of empty callback.
        auto step_cb3 = std::move(step_cb);
        REQUIRE(!step_cb3);
    }

    {
        auto lam = [](auto &) { return true; };

        step_callback<double> step_cb(lam);

        REQUIRE(static_cast<bool>(step_cb));

        REQUIRE(step_cb(ta));
        REQUIRE_NOTHROW(step_cb.pre_hook(ta));

#if !defined(_MSC_VER) || defined(__clang__)

        REQUIRE(step_cb.extract<int>() == nullptr);
        REQUIRE(std::as_const(step_cb).extract<int>() == nullptr);

#endif

        REQUIRE(step_cb.get_type_index() == typeid(decltype(lam)));

        REQUIRE(step_cb.extract<decltype(lam)>() != nullptr);
        REQUIRE(std::as_const(step_cb).extract<decltype(lam)>() != nullptr);

        // Copy construction.
        auto step_cb2 = step_cb;
        REQUIRE(step_cb2);
        REQUIRE(step_cb2.extract<decltype(lam)>() != nullptr);
        REQUIRE(step_cb2.extract<decltype(lam)>() != step_cb.extract<decltype(lam)>());

        // Move construction.
        auto step_cb3 = std::move(step_cb);
        REQUIRE(step_cb3);
        REQUIRE(step_cb3.extract<decltype(lam)>() != nullptr);
        REQUIRE(!step_cb);

        // Revive step_cb via copy assignment.
        step_cb = step_cb3;
        REQUIRE(step_cb);
        REQUIRE(step_cb.extract<decltype(lam)>() != nullptr);
        REQUIRE(step_cb.extract<decltype(lam)>() != step_cb3.extract<decltype(lam)>());

        // Revive step_cb via move assignment.
        const auto *orig_ptr = step_cb.extract<decltype(lam)>();
        auto step_cb4 = std::move(step_cb);
        step_cb = std::move(step_cb4);
        REQUIRE(!step_cb4);
        REQUIRE(step_cb.extract<decltype(lam)>() != nullptr);
        REQUIRE(step_cb.extract<decltype(lam)>() != step_cb3.extract<decltype(lam)>());
        REQUIRE(step_cb.extract<decltype(lam)>() == orig_ptr);
    }

    {
        step_callback<double> step_cb(&cb0<taylor_adaptive<double>>);

        REQUIRE(static_cast<bool>(step_cb));

        REQUIRE(step_cb.get_type_index() == typeid(decltype(&cb0<taylor_adaptive<double>>)));

        REQUIRE(step_cb(ta));
        REQUIRE_NOTHROW(step_cb.pre_hook(ta));
    }

    {
        step_callback<double> step_cb(cb0<taylor_adaptive<double>>);

        REQUIRE(static_cast<bool>(step_cb));

        REQUIRE(step_cb(ta));
        REQUIRE_NOTHROW(step_cb.pre_hook(ta));
    }

    {
        step_callback<double> step_cb(cb1{});

        REQUIRE(ta.get_state()[0] == 0.);

        REQUIRE(static_cast<bool>(step_cb));

        REQUIRE(!step_cb(ta));
        REQUIRE(ta.get_state()[0] == 2.);

        REQUIRE_NOTHROW(step_cb.pre_hook(ta));
        REQUIRE(ta.get_state()[0] == 1.);

        ta.get_state_data()[0] = 0;
    }

    {
        step_callback<double> step_cb([](auto &ta) {
            ta.get_state_data()[0] = 3;
            return true;
        });

        REQUIRE(ta.get_state()[0] == 0.);

        REQUIRE(static_cast<bool>(step_cb));

        REQUIRE(step_cb(ta));
        REQUIRE(ta.get_state()[0] == 3.);

        REQUIRE_NOTHROW(step_cb.pre_hook(ta));
        REQUIRE(ta.get_state()[0] == 3.);

        ta.get_state_data()[0] = 0;
    }

    {
        using std::swap;

        step_callback<double> step_cb1(cb1{}), step_cb2;

        swap(step_cb1, step_cb2);

        REQUIRE(static_cast<bool>(step_cb2));
        REQUIRE(!static_cast<bool>(step_cb1));

        REQUIRE(step_cb2.extract<cb1>() != nullptr);
        REQUIRE(step_cb1.extract<cb1>() == nullptr);
    }
}

struct cb2 {
    template <typename TA>
    bool operator()(TA &)
    {
        return false;
    }

    template <typename Archive>
    void serialize(Archive &, unsigned)
    {
    }
};

HEYOKA_S11N_STEP_CALLBACK_EXPORT(cb2, double)
HEYOKA_S11N_STEP_CALLBACK_BATCH_EXPORT(cb2, double)

TEST_CASE("step_callback s11n")
{
    {
        step_callback<double> step_cb(cb2{});

        std::stringstream ss;

        {
            boost::archive::binary_oarchive oa(ss);

            oa << step_cb;
        }

        step_cb = step_callback<double>{};
        REQUIRE(!step_cb);

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> step_cb;
        }

        REQUIRE(!!step_cb);
        REQUIRE(step_cb.extract<cb2>() != nullptr);
    }

    {
        step_callback<double> step_cb;

        std::stringstream ss;

        {
            boost::archive::binary_oarchive oa(ss);

            oa << step_cb;
        }

        step_cb = step_callback<double>{cb2{}};
        REQUIRE(step_cb);

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> step_cb;
        }

        REQUIRE(!step_cb);
    }

    {
        step_callback_batch<double> step_cb(cb2{});

        std::stringstream ss;

        {
            boost::archive::binary_oarchive oa(ss);

            oa << step_cb;
        }

        step_cb = step_callback_batch<double>{};
        REQUIRE(!step_cb);

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> step_cb;
        }

        REQUIRE(!!step_cb);
        REQUIRE(step_cb.extract<cb2>() != nullptr);
    }

    {
        step_callback_batch<double> step_cb;

        std::stringstream ss;

        {
            boost::archive::binary_oarchive oa(ss);

            oa << step_cb;
        }

        step_cb = step_callback_batch<double>{cb2{}};
        REQUIRE(step_cb);

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> step_cb;
        }

        REQUIRE(!step_cb);
    }
}

struct pend_cb {
    template <typename TA>
    bool operator()(TA &)
    {
        return true;
    }

    void pre_hook(taylor_adaptive<double> &ta)
    {
        ta.get_pars_data()[0] = 1.5;
    }

    void pre_hook(taylor_adaptive_batch<double> &ta)
    {
        ta.get_pars_data()[0] = 1.5;
        ta.get_pars_data()[1] = 1.5;
    }
};

struct tm_cb {
    template <typename TA>
    bool operator()(TA &)
    {
        return true;
    }

    void pre_hook(taylor_adaptive<double> &ta)
    {
        ta.set_time(ta.get_time() + 1);
    }

    void pre_hook(taylor_adaptive_batch<double> &ta)
    {
        ta.set_time({ta.get_time()[0] + 1, ta.get_time()[1] + 1});
    }
};

TEST_CASE("step_callback pre_hook")
{
    using Catch::Matchers::Message;

    auto dyn = model::pendulum(kw::l = par[0]);

    {
        auto ta0 = taylor_adaptive<double>{dyn, {1., 0.}};
        auto ta1 = taylor_adaptive<double>{dyn, {1., 0.}, kw::pars = {1.5}};

        REQUIRE(ta0.get_pars()[0] == 0.);

        ta0.propagate_until(3., kw::callback = pend_cb{});
        ta1.propagate_until(3.);

        REQUIRE(ta0.get_pars()[0] == 1.5);

        REQUIRE(ta0.get_state() == ta1.get_state());

        REQUIRE_THROWS_MATCHES(
            ta0.propagate_until(6., kw::callback = tm_cb{}), std::runtime_error,
            Message("The invocation of the callback passed to propagate_until() resulted in the alteration of the "
                    "time coordinate of the integrator - this is not supported"));

        REQUIRE(ta0.get_time() == 4);

        ta0.set_time(0.);
        ta0.get_pars_data()[0] = 0.1;
        ta1.set_time(0.);

        auto res0 = ta0.propagate_grid({0., 1., 2.}, kw::callback = pend_cb{});
        auto res1 = ta1.propagate_grid({0., 1., 2.});

        REQUIRE(std::get<4>(res0)[0] == std::get<4>(res1)[0]);

        REQUIRE(ta0.get_pars()[0] == 1.5);

        REQUIRE_THROWS_MATCHES(
            ta0.propagate_grid({3., 4.}, kw::callback = tm_cb{}), std::runtime_error,
            Message("The invocation of the callback passed to propagate_grid() resulted in the alteration of the "
                    "time coordinate of the integrator - this is not supported"));
    }

    {
        auto ta0 = taylor_adaptive_batch<double>{dyn, {1., 1.1, 0., 0.1}, 2u};
        auto ta1 = taylor_adaptive_batch<double>{dyn, {1., 1.1, 0., 0.1}, 2u, kw::pars = {1.5, 1.5}};

        REQUIRE(ta0.get_pars()[0] == 0.);
        REQUIRE(ta0.get_pars()[1] == 0.);

        ta0.propagate_until(3., kw::callback = pend_cb{});
        ta1.propagate_until(3.);

        REQUIRE(ta0.get_pars()[0] == 1.5);
        REQUIRE(ta0.get_pars()[1] == 1.5);

        REQUIRE(ta0.get_state() == ta1.get_state());

        REQUIRE_THROWS_MATCHES(
            ta0.propagate_until(6., kw::callback = tm_cb{}), std::runtime_error,
            Message("The invocation of the callback passed to propagate_until() resulted in the alteration of the "
                    "time coordinate of the integrator - this is not supported"));

        REQUIRE(ta0.get_time() == std::vector{4., 4.});

        ta0.set_time(0.);
        ta0.get_pars_data()[0] = 0.1;
        ta0.get_pars_data()[1] = 0.1;
        ta1.set_time(0.);

        auto res0 = ta0.propagate_grid({0., 0., 1., 1., 2., 2.}, kw::callback = pend_cb{});
        auto res1 = ta1.propagate_grid({0., 0., 1., 1., 2., 2.});

        REQUIRE(res0 == res1);

        REQUIRE(ta0.get_pars()[0] == 1.5);
        REQUIRE(ta0.get_pars()[1] == 1.5);

        REQUIRE_THROWS_MATCHES(
            ta0.propagate_grid({3., 3., 4., 4.}, kw::callback = tm_cb{}), std::runtime_error,
            Message("The invocation of the callback passed to propagate_grid() resulted in the alteration of the "
                    "time coordinate of the integrator - this is not supported"));
    }
}

TEST_CASE("step_callback_set")
{
    using Catch::Matchers::Message;
    using std::swap;

    auto dyn = model::pendulum();

    // Swappability.
    REQUIRE(std::is_nothrow_swappable_v<step_callback_set<double>>);
    REQUIRE(std::is_nothrow_swappable_v<step_callback_batch_set<double>>);

    // Basic API.
    step_callback_set<double> scs;
    REQUIRE(scs.size() == 0u);
    REQUIRE_THROWS_MATCHES(scs[0], std::out_of_range,
                           Message("Out of range index 0 when accessing a step callback set of size 0"));
    REQUIRE_THROWS_MATCHES(std::as_const(scs[0]), std::out_of_range,
                           Message("Out of range index 0 when accessing a step callback set of size 0"));
    auto scs2 = step_callback_set<double>{[](const auto &) { return true; }};
    REQUIRE(scs2.size() == 1u);
    REQUIRE_NOTHROW(scs2[0]);
    REQUIRE_NOTHROW(std::as_const(scs2)[0]);
    REQUIRE_THROWS_MATCHES(scs2[10], std::out_of_range,
                           Message("Out of range index 10 when accessing a step callback set of size 1"));

    swap(scs, scs2);
    REQUIRE(scs.size() == 1u);
    REQUIRE(scs2.size() == 0u);
    REQUIRE_NOTHROW(scs[0]);
    REQUIRE_NOTHROW(std::as_const(scs)[0]);
    REQUIRE_THROWS_MATCHES(scs[10], std::out_of_range,
                           Message("Out of range index 10 when accessing a step callback set of size 1"));

    auto scs3 = scs;
    REQUIRE(scs3.size() == 1u);

    auto scs4 = std::move(scs);
    REQUIRE(scs4.size() == 1u);

    scs = scs4;
    REQUIRE(scs.size() == 1u);

    scs2 = std::move(scs);
    REQUIRE(scs2.size() == 1u);

    // Empty set.
    {
        auto ta0 = taylor_adaptive<double>{dyn, {1., 0.}};

        const auto oc = std::get<0>(ta0.propagate_until(10., kw::callback = step_callback_set<double>{}));

        REQUIRE(oc == taylor_outcome::time_limit);
    }

    // Check sequencing of callback invocations.
    {
        int c1 = 0;
        int c2 = 0;

        auto ta0 = taylor_adaptive<double>{dyn, {1., 0.}};

        const auto oc
            = std::get<0>(ta0.propagate_until(10., kw::callback = step_callback_set<double>{[&c1, &c2](const auto &) {
                                                                                                REQUIRE(c1 == c2);
                                                                                                ++c1;
                                                                                                return true;
                                                                                            },
                                                                                            [&c1, &c2](const auto &) {
                                                                                                ++c2;
                                                                                                REQUIRE(c1 == c2);
                                                                                                return true;
                                                                                            }}));

        REQUIRE(oc == taylor_outcome::time_limit);
        REQUIRE(c1 == c2);
    }

    // Check stopping.
    {
        int c1 = 0;
        int c2 = 0;

        auto ta0 = taylor_adaptive<double>{dyn, {1., 0.}};

        const auto oc
            = std::get<0>(ta0.propagate_until(10., kw::callback = step_callback_set<double>{[&c1, &c2](const auto &) {
                                                                                                REQUIRE(c1 == c2);
                                                                                                ++c1;
                                                                                                return false;
                                                                                            },
                                                                                            [&c1, &c2](const auto &) {
                                                                                                ++c2;
                                                                                                REQUIRE(c1 == c2);
                                                                                                return true;
                                                                                            }}));

        REQUIRE(oc == taylor_outcome::cb_stop);
        REQUIRE(c1 == c2);
    }

    {
        int c1 = 0;
        int c2 = 0;

        auto ta0 = taylor_adaptive<double>{dyn, {1., 0.}};

        const auto oc
            = std::get<0>(ta0.propagate_until(10., kw::callback = step_callback_set<double>{[&c1, &c2](const auto &) {
                                                                                                REQUIRE(c1 == c2);
                                                                                                ++c1;
                                                                                                return true;
                                                                                            },
                                                                                            [&c1, &c2](const auto &) {
                                                                                                ++c2;
                                                                                                REQUIRE(c1 == c2);
                                                                                                return false;
                                                                                            }}));

        REQUIRE(oc == taylor_outcome::cb_stop);
        REQUIRE(c1 == c2);
    }

    // Pre-hook invocation.
    {
        int a = 0;
        int b = 0;

        int h1 = 0;
        int h2 = 0;

        struct my_cb1 {
            int &c1;
            int &c2;
            int &h;

            bool operator()(taylor_adaptive<double> &)
            {
                REQUIRE(c1 == c2);
                ++c1;
                return true;
            }

            void pre_hook(taylor_adaptive<double> &)
            {
                REQUIRE(h == 0);
                ++h;
            }
        };

        struct my_cb2 {
            int &c1;
            int &c2;
            int &h;

            bool operator()(taylor_adaptive<double> &)
            {
                ++c2;
                REQUIRE(c1 == c2);
                return true;
            }

            void pre_hook(taylor_adaptive<double> &)
            {
                REQUIRE(h == 0);
                ++h;
            }
        };

        auto ta0 = taylor_adaptive<double>{dyn, {1., 0.}};

        const auto oc = std::get<0>(
            ta0.propagate_until(10., kw::callback = step_callback_set<double>{my_cb1{a, b, h1}, my_cb2{a, b, h2}}));

        REQUIRE(oc == taylor_outcome::time_limit);
        REQUIRE(a == b);
        REQUIRE(h1 == 1);
        REQUIRE(h2 == 1);
    }

    // Error handling.
    {
        auto ta0 = taylor_adaptive<double>{dyn, {1., 0.}};

        REQUIRE_THROWS_MATCHES(
            ta0.propagate_until(6., kw::callback = step_callback_set<double>{step_callback<double>{}}),
            std::invalid_argument, Message("Cannot construct a callback set containing one or more empty callbacks"));
        REQUIRE_THROWS_MATCHES(
            ta0.propagate_until(6., kw::callback = step_callback_set<double>{step_callback<double>{},
                                                                             [](const auto &) { return true; }}),
            std::invalid_argument, Message("Cannot construct a callback set containing one or more empty callbacks"));
        REQUIRE_THROWS_MATCHES(
            ta0.propagate_until(6., kw::callback = step_callback_set<double>{[](const auto &) { return true; },
                                                                             step_callback<double>{}}),
            std::invalid_argument, Message("Cannot construct a callback set containing one or more empty callbacks"));
    }

    // Serialisation.
    {
        step_callback<double> scs{step_callback_set<double>{cb2{}}};

        std::stringstream ss;

        {
            boost::archive::binary_oarchive oa(ss);

            oa << scs;
        }

        scs = step_callback<double>{};
        REQUIRE(!scs);

        {
            boost::archive::binary_iarchive ia(ss);

            ia >> scs;
        }

        REQUIRE(static_cast<bool>(scs));
        REQUIRE(scs.extract<step_callback_set<double>>() != nullptr);
    }
}
