/*!
 * \file beidou_dnav_ephemeris.cc
 * \brief  Interface of a BeiDou EPHEMERIS storage and orbital model functions
 * \author Sergi Segura, 2018. sergi.segura.munoz(at)gmail.com
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2019  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -------------------------------------------------------------------------
 */

#include "beidou_dnav_ephemeris.h"
#include "Beidou_DNAV.h"
#include "gnss_satellite.h"
#include <cmath>


Beidou_Dnav_Ephemeris::Beidou_Dnav_Ephemeris()
{
    auto gnss_sat = Gnss_Satellite();
    std::string _system("Beidou");
    for (unsigned int i = 1; i < 36; i++)
        {
            satelliteBlock[i] = gnss_sat.what_block(_system, i);
        }
}


double Beidou_Dnav_Ephemeris::check_t(double time)
{
    double corrTime;
    double half_week = 302400.0;  // seconds
    corrTime = time;
    if (time > half_week)
        {
            corrTime = time - 2.0 * half_week;
        }
    else if (time < -half_week)
        {
            corrTime = time + 2.0 * half_week;
        }
    return corrTime;
}


double Beidou_Dnav_Ephemeris::sv_clock_drift(double transmitTime)
{
    double dt;
    dt = check_t(transmitTime - d_Toc);

    for (int i = 0; i < 2; i++)
        {
            dt -= d_A_f0 + d_A_f1 * dt + d_A_f2 * (dt * dt);
        }
    d_satClkDrift = d_A_f0 + d_A_f1 * dt + d_A_f2 * (dt * dt);

    return d_satClkDrift;
}


// compute the relativistic correction term
double Beidou_Dnav_Ephemeris::sv_clock_relativistic_term(double transmitTime)
{
    double tk;
    double a;
    double n;
    double n0;
    double E;
    double E_old;
    double dE;
    double M;

    // Restore semi-major axis
    a = d_sqrt_A * d_sqrt_A;

    // Time from ephemeris reference epoch
    tk = check_t(transmitTime - d_Toe);

    // Computed mean motion
    n0 = sqrt(BEIDOU_DNAV_GM / (a * a * a));
    // Corrected mean motion
    n = n0 + d_Delta_n;
    // Mean anomaly
    M = d_M_0 + n * tk;

    // Reduce mean anomaly to between 0 and 2pi
    M = fmod((M + 2.0 * BEIDOU_DNAV_PI), (2.0 * BEIDOU_DNAV_PI));

    // Initial guess of eccentric anomaly
    E = M;

    // --- Iteratively compute eccentric anomaly ----------------------------
    for (int ii = 1; ii < 20; ii++)
        {
            E_old = E;
            E = M + d_eccentricity * sin(E);
            dE = fmod(E - E_old, 2.0 * BEIDOU_DNAV_PI);
            if (fabs(dE) < 1e-12)
                {
                    // Necessary precision is reached, exit from the loop
                    break;
                }
        }

    // Compute relativistic correction term
    d_dtr = BEIDOU_DNAV_F * d_eccentricity * d_sqrt_A * sin(E);
    return d_dtr;
}


double Beidou_Dnav_Ephemeris::satellitePosition(double transmitTime)
{
    double tk;
    double a;
    double n;
    double n0;
    double M;
    double E;
    double E_old;
    double dE;
    double nu;
    double phi;
    double u;
    double r;
    double i;
    double Omega;

    // Find satellite's position ----------------------------------------------

    // Restore semi-major axis
    a = d_sqrt_A * d_sqrt_A;

    // Time from ephemeris reference epoch
    tk = check_t(transmitTime - d_Toe);

    // Computed mean motion
    n0 = sqrt(BEIDOU_DNAV_GM / (a * a * a));

    // Corrected mean motion
    n = n0 + d_Delta_n;

    // Mean anomaly
    M = d_M_0 + n * tk;

    // Reduce mean anomaly to between 0 and 2pi
    M = fmod((M + 2.0 * BEIDOU_DNAV_PI), (2.0 * BEIDOU_DNAV_PI));

    // Initial guess of eccentric anomaly
    E = M;

    // --- Iteratively compute eccentric anomaly ----------------------------
    for (int ii = 1; ii < 20; ii++)
        {
            E_old = E;
            E = M + d_eccentricity * sin(E);
            dE = fmod(E - E_old, 2.0 * BEIDOU_DNAV_PI);
            if (fabs(dE) < 1e-12)
                {
                    // Necessary precision is reached, exit from the loop
                    break;
                }
        }

    // Compute the true anomaly
    double tmp_Y = sqrt(1.0 - d_eccentricity * d_eccentricity) * sin(E);
    double tmp_X = cos(E) - d_eccentricity;
    nu = atan2(tmp_Y, tmp_X);

    // Compute angle phi (argument of Latitude)
    phi = nu + d_OMEGA;

    // Reduce phi to between 0 and 2*pi rad
    phi = fmod((phi), (2.0 * BEIDOU_DNAV_PI));

    // Correct argument of latitude
    u = phi + d_Cuc * cos(2.0 * phi) + d_Cus * sin(2.0 * phi);

    // Correct radius
    r = a * (1.0 - d_eccentricity * cos(E)) + d_Crc * cos(2.0 * phi) + d_Crs * sin(2.0 * phi);

    // Correct inclination
    i = d_i_0 + d_IDOT * tk + d_Cic * cos(2.0 * phi) + d_Cis * sin(2.0 * phi);

    // Compute the angle between the ascending node and the Greenwich meridian
    Omega = d_OMEGA0 + (d_OMEGA_DOT - BEIDOU_DNAV_OMEGA_EARTH_DOT) * tk - BEIDOU_DNAV_OMEGA_EARTH_DOT * d_Toe;

    // Reduce to between 0 and 2*pi rad
    Omega = fmod((Omega + 2.0 * BEIDOU_DNAV_PI), (2.0 * BEIDOU_DNAV_PI));

    // --- Compute satellite coordinates in Earth-fixed coordinates
    d_satpos_X = cos(u) * r * cos(Omega) - sin(u) * r * cos(i) * sin(Omega);
    d_satpos_Y = cos(u) * r * sin(Omega) + sin(u) * r * cos(i) * cos(Omega);
    d_satpos_Z = sin(u) * r * sin(i);

    // Satellite's velocity. Can be useful for Vector Tracking loops
    double Omega_dot = d_OMEGA_DOT - BEIDOU_DNAV_OMEGA_EARTH_DOT;
    d_satvel_X = -Omega_dot * (cos(u) * r + sin(u) * r * cos(i)) + d_satpos_X * cos(Omega) - d_satpos_Y * cos(i) * sin(Omega);
    d_satvel_Y = Omega_dot * (cos(u) * r * cos(Omega) - sin(u) * r * cos(i) * sin(Omega)) + d_satpos_X * sin(Omega) + d_satpos_Y * cos(i) * cos(Omega);
    d_satvel_Z = d_satpos_Y * sin(i);

    // Time from ephemeris reference clock
    tk = check_t(transmitTime - d_Toc);

    double dtr_s = d_A_f0 + d_A_f1 * tk + d_A_f2 * tk * tk;

    /* relativity correction */
    dtr_s -= 2.0 * sqrt(BEIDOU_DNAV_GM * a) * d_eccentricity * sin(E) / (BEIDOU_DNAV_C_M_S * BEIDOU_DNAV_C_M_S);

    return dtr_s;
}
