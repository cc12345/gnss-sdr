/*!
 * \file kml_printer.h
 * \brief Interface of a class that prints PVT information to a kml file
 * \author Javier Arribas, 2011. jarribas(at)cttc.es
 *         Álvaro Cebrián Juan, 2018. acebrianjuan(at)gmail.com
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


#ifndef GNSS_SDR_KML_PRINTER_H
#define GNSS_SDR_KML_PRINTER_H

#include <fstream>  // for ofstream
#include <string>

class Pvt_Solution;

/*!
 * \brief Prints PVT information to OGC KML format file (can be viewed with Google Earth)
 *
 * See https://www.opengeospatial.org/standards/kml
 */
class Kml_Printer
{
public:
    explicit Kml_Printer(const std::string& base_path = std::string("."));
    ~Kml_Printer();
    bool set_headers(const std::string& filename, bool time_tag_name = true);
    bool print_position(const Pvt_Solution* position, bool print_average_values);
    bool close_file();

private:
    std::ofstream kml_file;
    std::ofstream tmp_file;
    std::string kml_filename;
    std::string kml_base_path;
    std::string tmp_file_str;
    std::string indent;
    unsigned int point_id;
    bool positions_printed;
};

#endif
