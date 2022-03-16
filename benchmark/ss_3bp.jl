using DiffEqBase
using BenchmarkTools
using OrdinaryDiffEq

const G = 0.01720209895 * 0.01720209895 * 365 * 365
const m_sun = 1.00000597682
const m_jup = 1 / 1047.355
const m_sat = 1 / 3501.6

const tol = 1e-15

const with_dense = true

function compute_energy(st)
    m0 = m_sun
    m1 = m_jup
    m2 = m_sat

    x0 = st[0]
    y0 = st[1]
    z0 = st[2]
    vx0 = st[3]
    vy0 = st[4]
    vz0 = st[5]

    x1 = st[6]
    y1 = st[7]
    z1 = st[8]
    vx1 = st[9]
    vy1 = st[10]
    vz1 = st[11]

    x2 = st[12]
    y2 = st[13]
    z2 = st[14]
    vx2 = st[15]
    vy2 = st[16]
    vz2 = st[17]

    v0_2 = vx0 * vx0 + vy0 * vy0 + vz0 * vz0
    v1_2 = vx1 * vx1 + vy1 * vy1 + vz1 * vz1
    v2_2 = vx2 * vx2 + vy2 * vy2 + vz2 * vz2

    K = 0.5 * (m0 * v0_2 + m1 * v1_2 + m2 * v2_2)

    x01 = x0 - x1
    y01 = y0 - y1
    z01 = z0 - z1
    x02 = x0 - x2
    y02 = y0 - y2
    z02 = z0 - z2
    x12 = x1 - x2
    y12 = y1 - y2
    z12 = z1 - z2

    r01_2 = x01 * x01 + y01 * y01 + z01 * z01
    r02_2 = x02 * x02 + y02 * y02 + z02 * z02
    r12_2 = x12 * x12 + y12 * y12 + z12 * z12

    U = -G * (m0 * m1 / sqrt(r01_2) + m0 * m2 / sqrt(r02_2) + m1 * m2 / sqrt(r12_2))

    return K + U
end

function ss_3bp!(dq, q, param, t)
    x_sun, y_sun, z_sun, vx_sun, vy_sun, vz_sun, x_jup, y_jup, z_jup, vx_jup, vy_jup, vz_jup, x_sat, y_sat, z_sat, vx_sat, vy_sat, vz_sat = q

    dq[1] = vx_sun
    dq[2] = vy_sun
    dq[3] = vz_sun

    dq[7] = vx_jup
    dq[8] = vy_jup
    dq[9] = vz_jup

    dq[13] = vx_sat
    dq[14] = vy_sat
    dq[15] = vz_sat

    diff_x_sun_jup = x_sun - x_jup
    diff_y_sun_jup = y_sun - y_jup
    diff_z_sun_jup = z_sun - z_jup
    r_sun_jup3_tmp = sqrt(diff_x_sun_jup * diff_x_sun_jup + diff_y_sun_jup * diff_y_sun_jup + diff_z_sun_jup * diff_z_sun_jup)
    r_sun_jup3 = r_sun_jup3_tmp * r_sun_jup3_tmp * r_sun_jup3_tmp

    diff_x_sun_sat = x_sun - x_sat
    diff_y_sun_sat = y_sun - y_sat
    diff_z_sun_sat = z_sun - z_sat
    r_sun_sat3_tmp = sqrt(diff_x_sun_sat * diff_x_sun_sat + diff_y_sun_sat * diff_y_sun_sat
                      + diff_z_sun_sat * diff_z_sun_sat)
    r_sun_sat3 = r_sun_sat3_tmp * r_sun_sat3_tmp * r_sun_sat3_tmp

    diff_x_jup_sat = x_jup - x_sat
    diff_y_jup_sat = y_jup - y_sat
    diff_z_jup_sat = z_jup - z_sat
    r_jup_sat3_tmp = sqrt(diff_x_jup_sat * diff_x_jup_sat + diff_y_jup_sat * diff_y_jup_sat
                      + diff_z_jup_sat * diff_z_jup_sat)
    r_jup_sat3 = r_jup_sat3_tmp * r_jup_sat3_tmp * r_jup_sat3_tmp

    dq[4] = -G * m_jup * diff_x_sun_jup / r_sun_jup3 - G * m_sat * diff_x_sun_sat / r_sun_sat3
    dq[5] = -G * m_jup * diff_y_sun_jup / r_sun_jup3 - G * m_sat * diff_y_sun_sat / r_sun_sat3
    dq[6] = -G * m_jup * diff_z_sun_jup / r_sun_jup3 - G * m_sat * diff_z_sun_sat / r_sun_sat3

    dq[10] = G * m_sun * diff_x_sun_jup / r_sun_jup3 - G * m_sat * diff_x_jup_sat / r_jup_sat3
    dq[11] = G * m_sun * diff_y_sun_jup / r_sun_jup3 - G * m_sat * diff_y_jup_sat / r_jup_sat3
    dq[12] = G * m_sun * diff_z_sun_jup / r_sun_jup3 - G * m_sat * diff_z_jup_sat / r_jup_sat3

    dq[16] = G * m_sun * diff_x_sun_sat / r_sun_sat3 + G * m_jup * diff_x_jup_sat / r_jup_sat3
    dq[17] = G * m_sun * diff_y_sun_sat / r_sun_sat3 + G * m_jup * diff_y_jup_sat / r_jup_sat3
    dq[18] = G * m_sun * diff_z_sun_sat / r_sun_sat3 + G * m_jup * diff_z_jup_sat / r_jup_sat3

    return nothing
end

tspan = (0.0, 100000.0);

q0 = [-5.137271893918405e-03, -5.288891104344273e-03, 6.180743702483316e-06, 2.3859757364179156e-03,
      -2.3396779489468049e-03, -8.1384891821122709e-07,
      3.404393156051084, 3.6305811472186558, 0.0342464685434024, -2.0433186406983279e+00,
      2.0141003472039567e+00, -9.7316724504621210e-04,
      6.606942557811084, 6.381645992310656, -0.1361381213577972, -1.5233982268351876, 1.4589658329821569,
      0.0061033600397708];

println(compute_energy(q0))

ref = [0.00529783352211642635986448512870267126,   0.00443801687663031764860286477965782292,
      -9.26927271770180624859940898715912866e-07, -0.00216531823419081350439333313986268641,
      0.00214032867924028123522294255311480690,   -8.67721509026440812519470549521477867e-06,
      -2.92497467718230374582976002426387292,     -4.36042856694508689271852041970916660,
      -0.0347154846850475113508604212948948637,   2.21048191345880323553795415392017512,
      -1.58848197474132095629314575660274164,     0.00454279037439684585301286724874262847,
      -8.77199825846701098536240728390136140,     -0.962123421465639669245658270137496032,
      0.119309299617985001156428107644659796,     0.191865835965930410443458136955585028,
      -2.18388123410681074311949955152592019,     0.0151965022008957683497311770695583142];

prob = ODEProblem(ss_3bp!, q0, tspan);

if with_dense
    grid = LinRange(0., 100000., 500000);

    sol = solve(prob, Vern9(), abstol=tol, reltol=tol, saveat=grid);
    err = ref - sol.u[length(sol.u)];
    println(sqrt(sum(err.^2.) / length(err)))
    bV = @benchmark solve($prob, $(Vern9()), abstol=tol, reltol=tol, saveat=grid)
    println(bV)
else
    sol = solve(prob, Vern9(), abstol=tol, reltol=tol, save_everystep=false);
    err = ref - sol.u[length(sol.u)];
    println(sqrt(sum(err.^2.) / length(err)))
    bV = @benchmark solve($prob, $(Vern9()), abstol=tol, reltol=tol, save_everystep=false)
    println(bV)
end
