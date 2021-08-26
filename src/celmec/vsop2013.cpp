// Copyright 2020, 2021 Francesco Biscani (bluescarni@gmail.com), Dario Izzo (dario.izzo@gmail.com)
//
// This file is part of the heyoka library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/numeric/conversion/cast.hpp>
#include <boost/version.hpp>

// NOTE: the header for hash_combine changed in version 1.67.
#if (BOOST_VERSION / 100000 > 1) || (BOOST_VERSION / 100000 == 1 && BOOST_VERSION / 100 % 1000 >= 67)

#include <boost/container_hash/hash.hpp>

#else

#include <boost/functional/hash.hpp>

#endif

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <fmt/format.h>

#include <heyoka/celmec/vsop2013.hpp>
#include <heyoka/detail/vsop2013/vsop2013_1.hpp>
#include <heyoka/detail/vsop2013/vsop2013_2.hpp>
#include <heyoka/detail/vsop2013/vsop2013_3.hpp>
#include <heyoka/detail/vsop2013/vsop2013_4.hpp>
#include <heyoka/detail/vsop2013/vsop2013_5.hpp>
#include <heyoka/detail/vsop2013/vsop2013_6.hpp>
#include <heyoka/detail/vsop2013/vsop2013_7.hpp>
#include <heyoka/detail/vsop2013/vsop2013_8.hpp>
#include <heyoka/detail/vsop2013/vsop2013_9.hpp>
#include <heyoka/expression.hpp>
#include <heyoka/math/atan2.hpp>
#include <heyoka/math/cos.hpp>
#include <heyoka/math/kepE.hpp>
#include <heyoka/math/pow.hpp>
#include <heyoka/math/sin.hpp>
#include <heyoka/math/sqrt.hpp>
#include <heyoka/math/square.hpp>

#if defined(_MSC_VER) && !defined(__clang__)

// NOTE: MSVC has issues with the other "using"
// statement form.
using namespace fmt::literals;

#else

using fmt::literals::operator""_format;

#endif

namespace heyoka::detail
{

namespace
{

// Hasher for use in the map below.
struct vsop2013_hasher {
    std::size_t operator()(const std::pair<std::uint32_t, std::uint32_t> &p) const
    {
        std::size_t seed = std::hash<std::uint32_t>{}(p.first);
        boost::hash_combine(seed, p.second);
        return seed;
    }
};

// This dictionary will map a pair of indices (i, j)
// (planet index and variable index) to a tuple containing:
// - the maximum value of alpha (the time power) + 1,
// - a pointer to an array containing the sizes of the series'
//   chunks for each value of alpha,
// - a pointer to an array of pointers, i.e., a jagged 2D array
//   in which each row contains the data necessary to build
//   the chunks for each value of alpha, from 0 to max_alpha.
using vsop2013_data_t
    = std::unordered_map<std::pair<std::uint32_t, std::uint32_t>,
                         std::tuple<std::size_t, const unsigned long *, const double *const *>, vsop2013_hasher>;

// Helper to construct the data dictionary that
// we will be querying for constructing the series
// at runtime.
auto build_vsop2103_data()
{
    vsop2013_data_t retval;

#define HEYOKA_VSOP2013_RECORD_DATA(pl_idx, var_idx)                                                                   \
    retval[{pl_idx, var_idx}]                                                                                          \
        = std::tuple{std::size(vsop2013_##pl_idx##_##var_idx##_sizes), &vsop2013_##pl_idx##_##var_idx##_sizes[0],      \
                     &vsop2013_##pl_idx##_##var_idx##_data[0]}

    HEYOKA_VSOP2013_RECORD_DATA(1, 1);
    HEYOKA_VSOP2013_RECORD_DATA(1, 2);
    HEYOKA_VSOP2013_RECORD_DATA(1, 3);
    HEYOKA_VSOP2013_RECORD_DATA(1, 4);
    HEYOKA_VSOP2013_RECORD_DATA(1, 5);
    HEYOKA_VSOP2013_RECORD_DATA(1, 6);

    HEYOKA_VSOP2013_RECORD_DATA(2, 1);
    HEYOKA_VSOP2013_RECORD_DATA(2, 2);
    HEYOKA_VSOP2013_RECORD_DATA(2, 3);
    HEYOKA_VSOP2013_RECORD_DATA(2, 4);
    HEYOKA_VSOP2013_RECORD_DATA(2, 5);
    HEYOKA_VSOP2013_RECORD_DATA(2, 6);

    HEYOKA_VSOP2013_RECORD_DATA(3, 1);
    HEYOKA_VSOP2013_RECORD_DATA(3, 2);
    HEYOKA_VSOP2013_RECORD_DATA(3, 3);
    HEYOKA_VSOP2013_RECORD_DATA(3, 4);
    HEYOKA_VSOP2013_RECORD_DATA(3, 5);
    HEYOKA_VSOP2013_RECORD_DATA(3, 6);

    HEYOKA_VSOP2013_RECORD_DATA(4, 1);
    HEYOKA_VSOP2013_RECORD_DATA(4, 2);
    HEYOKA_VSOP2013_RECORD_DATA(4, 3);
    HEYOKA_VSOP2013_RECORD_DATA(4, 4);
    HEYOKA_VSOP2013_RECORD_DATA(4, 5);
    HEYOKA_VSOP2013_RECORD_DATA(4, 6);

    HEYOKA_VSOP2013_RECORD_DATA(5, 1);
    HEYOKA_VSOP2013_RECORD_DATA(5, 2);
    HEYOKA_VSOP2013_RECORD_DATA(5, 3);
    HEYOKA_VSOP2013_RECORD_DATA(5, 4);
    HEYOKA_VSOP2013_RECORD_DATA(5, 5);
    HEYOKA_VSOP2013_RECORD_DATA(5, 6);

    HEYOKA_VSOP2013_RECORD_DATA(6, 1);
    HEYOKA_VSOP2013_RECORD_DATA(6, 2);
    HEYOKA_VSOP2013_RECORD_DATA(6, 3);
    HEYOKA_VSOP2013_RECORD_DATA(6, 4);
    HEYOKA_VSOP2013_RECORD_DATA(6, 5);
    HEYOKA_VSOP2013_RECORD_DATA(6, 6);

    HEYOKA_VSOP2013_RECORD_DATA(7, 1);
    HEYOKA_VSOP2013_RECORD_DATA(7, 2);
    HEYOKA_VSOP2013_RECORD_DATA(7, 3);
    HEYOKA_VSOP2013_RECORD_DATA(7, 4);
    HEYOKA_VSOP2013_RECORD_DATA(7, 5);
    HEYOKA_VSOP2013_RECORD_DATA(7, 6);

    HEYOKA_VSOP2013_RECORD_DATA(8, 1);
    HEYOKA_VSOP2013_RECORD_DATA(8, 2);
    HEYOKA_VSOP2013_RECORD_DATA(8, 3);
    HEYOKA_VSOP2013_RECORD_DATA(8, 4);
    HEYOKA_VSOP2013_RECORD_DATA(8, 5);
    HEYOKA_VSOP2013_RECORD_DATA(8, 6);

    HEYOKA_VSOP2013_RECORD_DATA(9, 1);
    HEYOKA_VSOP2013_RECORD_DATA(9, 2);
    HEYOKA_VSOP2013_RECORD_DATA(9, 3);
    HEYOKA_VSOP2013_RECORD_DATA(9, 4);
    HEYOKA_VSOP2013_RECORD_DATA(9, 5);
    HEYOKA_VSOP2013_RECORD_DATA(9, 6);

#undef HEYOKA_VSOP2013_RECORD_DATA

    return retval;
}

} // namespace

// Implementation of the function constructing the VSOP2013 elliptic series as heyoka expressions. The elements
// are referred to the Dynamical Frame J2000.
expression vsop2013_elliptic_impl(std::uint32_t pl_idx, std::uint32_t var_idx, expression t_expr, double thresh)
{
    // Check the input values.
    if (pl_idx < 1u || pl_idx > 9u) {
        throw std::invalid_argument("Invalid planet index passed to vsop2013_elliptic(): "
                                    "the index must be in the [1, 9] range, but it is {} instead"_format(pl_idx));
    }

    if (var_idx < 1u || var_idx > 6u) {
        throw std::invalid_argument("Invalid variable index passed to vsop2013_elliptic(): "
                                    "the index must be in the [1, 6] range, but it is {} instead"_format(var_idx));
    }

    if (!std::isfinite(thresh) || thresh < 0.) {
        throw std::invalid_argument("Invalid threshold value passed to vsop2013_elliptic(): "
                                    "the value must be finite and non-negative, but it is {} instead"_format(thresh));
    }

    // The lambda_l values (constant + linear term).
    constexpr std::array<std::array<double, 2>, 17> lam_l_data = {{{4.402608631669, 26087.90314068555},
                                                                   {3.176134461576, 10213.28554743445},
                                                                   {1.753470369433, 6283.075850353215},
                                                                   {6.203500014141, 3340.612434145457},
                                                                   {4.091360003050, 1731.170452721855},
                                                                   {1.713740719173, 1704.450855027201},
                                                                   {5.598641292287, 1428.948917844273},
                                                                   {2.805136360408, 1364.756513629990},
                                                                   {2.326989734620, 1361.923207632842},
                                                                   {0.599546107035, 529.6909615623250},
                                                                   {0.874018510107, 213.2990861084880},
                                                                   {5.481225395663, 74.78165903077800},
                                                                   {5.311897933164, 38.13297222612500},
                                                                   {0, 0.3595362285049309},
                                                                   {5.198466400630, 77713.7714481804},
                                                                   {1.627905136020, 84334.6615717837},
                                                                   {2.355555638750, 83286.9142477147}}};

    // Fetch the data.
    static const auto data = build_vsop2103_data();

    // Locate the data entry for the current planet and variable.
    const auto data_it = data.find({pl_idx, var_idx});
    assert(data_it != data.end()); // LCOV_EXCL_LINE
    const auto [n_alpha, sizes_ptr, val_ptr] = data_it->second;

    // This vector will contain the chunks of the series
    // for different values of alpha.
    std::vector<expression> parts(boost::numeric_cast<std::vector<expression>::size_type>(n_alpha));

    tbb::parallel_for(tbb::blocked_range(std::size_t(0), n_alpha), [&](const auto &r) {
        for (auto alpha = r.begin(); alpha != r.end(); ++alpha) {
            // Fetch the number of terms for this chunk.
            const auto cur_size = sizes_ptr[alpha];

            // This vector will contain the terms of the chunk
            // for the current value of alpha.
            std::vector<expression> cur(boost::numeric_cast<std::vector<expression>::size_type>(cur_size));

            tbb::parallel_for(tbb::blocked_range(std::size_t(0), cur_size), [&](const auto &r_in) {
                // trig will contain the components of the
                // sin/cos trigonometric argument.
                auto trig = std::vector<expression>(17u);

                for (auto i = r_in.begin(); i != r_in.end(); ++i) {
                    // Load the C/S values from the table.
                    const auto Sval = val_ptr[alpha][i * 19u + 17u];
                    const auto Cval = val_ptr[alpha][i * 19u + 18u];

                    // Check if we have reached a term which is too small.
                    if (std::sqrt(Cval * Cval + Sval * Sval) < thresh) {
                        break;
                    }

                    for (std::size_t j = 0; j < 17u; ++j) {
                        // Compute lambda_l for the current element
                        // of the trigonometric argument.
                        auto cur_lam = lam_l_data[j][0] + t_expr * lam_l_data[j][1];

                        // Multiply it by the current value in the table.
                        trig[j] = std::move(cur_lam) * val_ptr[alpha][i * 19u + j];
                    }

                    // Compute the trig arg.
                    auto trig_arg = pairwise_sum(trig);

                    // Add the term to the chunk.
                    auto tmp = Sval * sin(trig_arg);
                    cur[i] = std::move(tmp) + Cval * cos(std::move(trig_arg));
                }
            });

            // Sum the terms in the chunk and multiply them by t**alpha.
            parts[alpha] = powi(t_expr, boost::numeric_cast<std::uint32_t>(alpha)) * pairwise_sum(std::move(cur));
        }
    });

    // Sum the chunks and return them.
    return pairwise_sum(std::move(parts));
}

// Implementation of the function constructing the VSOP2013 Cartesian series as heyoka expressions. The coordinates
// are referred to the Dynamical Frame J2000.
std::vector<expression> vsop2013_cartesian_impl(std::uint32_t pl_idx, expression t_expr, double thresh)
{
    // Get the elliptic orbital elements.
    const auto a = vsop2013_elliptic_impl(pl_idx, 1, t_expr, thresh);
    const auto lam = vsop2013_elliptic_impl(pl_idx, 2, t_expr, thresh);
    const auto k = vsop2013_elliptic_impl(pl_idx, 3, t_expr, thresh);
    const auto h = vsop2013_elliptic_impl(pl_idx, 4, t_expr, thresh);
    const auto q = vsop2013_elliptic_impl(pl_idx, 5, t_expr, thresh);
    const auto p = vsop2013_elliptic_impl(pl_idx, 6, t_expr, thresh);

    // e.
    const auto e = sqrt(square(k) + square(h));

    // sqrt(1 - e**2).
    const auto sqrt_1me2 = sqrt(1_dbl - (square(k) + square(h)));

    // cos(i)/sin(i).
    const auto ci = 1_dbl - 2_dbl * (square(q) + square(p));
    const auto si = sqrt(1_dbl - square(ci));

    // cos(Om)/sin(Om).
    const auto cOm = q / sqrt(square(q) + square(p));
    const auto sOm = p / sqrt(square(q) + square(p));

    // cos(om)/sin(om).
    const auto com = (k * cOm + h * sOm) / e;
    const auto som = (h * cOm - k * sOm) / e;

    // M.
    const auto M = lam - atan2(h, k);

    // E.
    const auto E = kepE(e, M);

    // q1/a and q2/a.
    const auto q1_a = cos(E) - e;
    const auto q2_a = sqrt_1me2 * sin(E);

    // Prepare the return value.
    std::vector<expression> retval;

    // x.
    retval.push_back(a * (q1_a * (cOm * com - sOm * ci * som) - q2_a * (cOm * som + sOm * ci * com)));

    // y.
    retval.push_back(a * (q1_a * (sOm * com + cOm * ci * som) - q2_a * (sOm * som - cOm * ci * com)));

    // z.
    retval.push_back(a * (q1_a * (si * som) + q2_a * (si * com)));

    // G*M values for the planets.
    constexpr double gm_pl[] = {4.9125474514508118699e-11, 7.2434524861627027000e-10, 8.9970116036316091182e-10,
                                9.5495351057792580598e-11, 2.8253458420837780000e-07, 8.4597151856806587398e-08,
                                1.2920249167819693900e-08, 1.5243589007842762800e-08, 2.1886997654259696800e-12};

    // G*M value for the Sun.
    constexpr double gm_sun = 2.9591220836841438269e-04;

    // Compute the gravitational parameter for pl_idx.
    assert(pl_idx >= 1u && pl_idx <= 9u); // LCOV_EXCL_LINE
    const auto mu = std::sqrt(gm_sun + gm_pl[pl_idx - 1u]);

    // vx.
    retval.push_back(mu * (-sin(E) * (cOm * com - sOm * ci * som) - sqrt_1me2 * cos(E) * (cOm * som + sOm * ci * com))
                     / (sqrt(a) * (1_dbl - e * cos(E))));

    // vy.
    retval.push_back(mu * (-sin(E) * (sOm * com + cOm * ci * som) - sqrt_1me2 * cos(E) * (sOm * som - cOm * ci * com))
                     / (sqrt(a) * (1_dbl - e * cos(E))));

    // vz.
    retval.push_back(mu * (-sin(E) * (si * som) + sqrt_1me2 * cos(E) * (si * com)) / (sqrt(a) * (1_dbl - e * cos(E))));

    return retval;
}

// Implementation of the function constructing the VSOP2013 Cartesian series as heyoka expressions. The coordinates
// are referred to ICRF frame.
std::vector<expression> vsop2013_cartesian_icrf_impl(std::uint32_t pl_idx, expression t_expr, double thresh)
{
    // Compute the Cartesian coordinates in the Dynamical Frame J2000.
    const auto cart_dfj2000 = vsop2013_cartesian_impl(pl_idx, std::move(t_expr), thresh);

    // The two rotation angles for the transition Dynamical Frame J2000 -> ICRF.
    const auto eps = 0.4090926265865962;
    const auto phi = -2.5152133775962285e-07;

    // Perform the rotation.
    const auto &xe = cart_dfj2000[0];
    const auto &ye = cart_dfj2000[1];
    const auto &ze = cart_dfj2000[2];
    const auto &vxe = cart_dfj2000[3];
    const auto &vye = cart_dfj2000[4];
    const auto &vze = cart_dfj2000[5];

    std::vector<expression> retval;
    retval.push_back(std::cos(phi) * xe - std::sin(phi) * std::cos(eps) * ye + std::sin(phi) * std::sin(eps) * ze);
    retval.push_back(std::sin(phi) * xe + std::cos(phi) * std::cos(eps) * ye - std::cos(phi) * std::sin(eps) * ze);
    retval.push_back(std::sin(eps) * ye + std::cos(eps) * ze);
    retval.push_back(std::cos(phi) * vxe - std::sin(phi) * std::cos(eps) * vye + std::sin(phi) * std::sin(eps) * vze);
    retval.push_back(std::sin(phi) * vxe + std::cos(phi) * std::cos(eps) * vye - std::cos(phi) * std::sin(eps) * vze);
    retval.push_back(std::sin(eps) * vye + std::cos(eps) * vze);

    return retval;
}

} // namespace heyoka::detail
