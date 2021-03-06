/*!
 * \file gps_l5_telemetry_decoder_gs.cc
 * \brief Implementation of a CNAV message demodulator block
 * \author Antonio Ramos, 2017. antonio.ramos(at)cttc.es
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


#include "gps_l5_telemetry_decoder_gs.h"
#include "display.h"
#include "gnss_synchro.h"
#include "gps_cnav_ephemeris.h"
#include "gps_cnav_iono.h"
#include "gps_cnav_utc_model.h"  // for Gps_CNAV_Utc_Model
#include <glog/logging.h>
#include <gnuradio/io_signature.h>
#include <pmt/pmt.h>        // for make_any
#include <pmt/pmt_sugar.h>  // for mp
#include <bitset>           // for std::bitset
#include <cstdlib>          // for std::llabs
#include <exception>        // for std::exception
#include <iostream>         // for std::cout
#include <memory>           // for shared_ptr, make_shared


gps_l5_telemetry_decoder_gs_sptr
gps_l5_make_telemetry_decoder_gs(const Gnss_Satellite &satellite, bool dump)
{
    return gps_l5_telemetry_decoder_gs_sptr(new gps_l5_telemetry_decoder_gs(satellite, dump));
}


gps_l5_telemetry_decoder_gs::gps_l5_telemetry_decoder_gs(
    const Gnss_Satellite &satellite, bool dump) : gr::block("gps_l5_telemetry_decoder_gs",
                                                      gr::io_signature::make(1, 1, sizeof(Gnss_Synchro)),
                                                      gr::io_signature::make(1, 1, sizeof(Gnss_Synchro)))
{
    // prevent telemetry symbols accumulation in output buffers
    this->set_max_noutput_items(1);
    // Ephemeris data port out
    this->message_port_register_out(pmt::mp("telemetry"));
    // Control messages to tracking block
    this->message_port_register_out(pmt::mp("telemetry_to_trk"));
    d_last_valid_preamble = 0;
    d_sent_tlm_failed_msg = false;
    d_max_symbols_without_valid_frame = GPS_L5_CNAV_DATA_PAGE_BITS * GPS_L5_SYMBOLS_PER_BIT * 10;  // rise alarm if 20 consecutive subframes have no valid CRC

    // initialize internal vars
    d_dump = dump;
    d_satellite = Gnss_Satellite(satellite.get_system(), satellite.get_PRN());
    DLOG(INFO) << "GPS L5 TELEMETRY PROCESSING: satellite " << d_satellite;
    d_channel = 0;
    d_flag_valid_word = false;
    d_TOW_at_current_symbol_ms = 0U;
    d_TOW_at_Preamble_ms = 0U;
    // initialize the CNAV frame decoder (libswiftcnav)
    cnav_msg_decoder_init(&d_cnav_decoder);

    d_sample_counter = 0;
    d_flag_PLL_180_deg_phase_locked = false;
}


gps_l5_telemetry_decoder_gs::~gps_l5_telemetry_decoder_gs()
{
    DLOG(INFO) << "GPS L5 Telemetry decoder block (channel " << d_channel << ") destructor called.";
    if (d_dump_file.is_open() == true)
        {
            try
                {
                    d_dump_file.close();
                }
            catch (const std::exception &ex)
                {
                    LOG(WARNING) << "Exception in destructor closing the dump file " << ex.what();
                }
        }
}


void gps_l5_telemetry_decoder_gs::set_satellite(const Gnss_Satellite &satellite)
{
    d_satellite = Gnss_Satellite(satellite.get_system(), satellite.get_PRN());
    DLOG(INFO) << "GPS L5 CNAV telemetry decoder in channel " << this->d_channel << " set to satellite " << d_satellite;
    d_CNAV_Message = Gps_CNAV_Navigation_Message();
}


void gps_l5_telemetry_decoder_gs::set_channel(int32_t channel)
{
    d_channel = channel;
    d_CNAV_Message = Gps_CNAV_Navigation_Message();
    DLOG(INFO) << "GPS L5 CNAV channel set to " << channel;
    // ############# ENABLE DATA FILE LOG #################
    if (d_dump == true)
        {
            if (d_dump_file.is_open() == false)
                {
                    try
                        {
                            d_dump_filename = "telemetry_L5_";
                            d_dump_filename.append(std::to_string(d_channel));
                            d_dump_filename.append(".dat");
                            d_dump_file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
                            d_dump_file.open(d_dump_filename.c_str(), std::ios::out | std::ios::binary);
                            LOG(INFO) << "Telemetry decoder dump enabled on channel " << d_channel
                                      << " Log file: " << d_dump_filename.c_str();
                        }
                    catch (const std::ifstream::failure &e)
                        {
                            LOG(WARNING) << "channel " << d_channel << " Exception opening Telemetry GPS L5 dump file " << e.what();
                        }
                }
        }
}


void gps_l5_telemetry_decoder_gs::reset()
{
    d_last_valid_preamble = d_sample_counter;
    d_TOW_at_current_symbol_ms = 0;
    d_sent_tlm_failed_msg = false;
    d_flag_valid_word = false;
    DLOG(INFO) << "Telemetry decoder reset for satellite " << d_satellite;
}


int gps_l5_telemetry_decoder_gs::general_work(int noutput_items __attribute__((unused)), gr_vector_int &ninput_items __attribute__((unused)),
    gr_vector_const_void_star &input_items, gr_vector_void_star &output_items)
{
    // get pointers on in- and output gnss-synchro objects
    auto *out = reinterpret_cast<Gnss_Synchro *>(output_items[0]);            // Get the output buffer pointer
    const auto *in = reinterpret_cast<const Gnss_Synchro *>(input_items[0]);  // Get the input buffer pointer

    // UPDATE GNSS SYNCHRO DATA
    Gnss_Synchro current_synchro_data{};  // structure to save the synchronization information and send the output object to the next block
    // 1. Copy the current tracking output
    current_synchro_data = in[0];
    consume_each(1);  // one by one

    // check if there is a problem with the telemetry of the current satellite
    d_sample_counter++;  // count for the processed symbols
    if (d_sent_tlm_failed_msg == false)
        {
            if ((d_sample_counter - d_last_valid_preamble) > d_max_symbols_without_valid_frame)
                {
                    int message = 1;  // bad telemetry
                    this->message_port_pub(pmt::mp("telemetry_to_trk"), pmt::make_any(message));
                    d_sent_tlm_failed_msg = true;
                }
        }

    cnav_msg_t msg;
    uint32_t delay;
    uint8_t symbol_clip = static_cast<uint8_t>(current_synchro_data.Prompt_Q > 0) * 255;
    // 2. Add the telemetry decoder information
    // check if new CNAV frame is available
    if (cnav_msg_decoder_add_symbol(&d_cnav_decoder, symbol_clip, &msg, &delay) == true)
        {
            if (d_cnav_decoder.part1.invert == true or d_cnav_decoder.part2.invert == true)
                {
                    d_flag_PLL_180_deg_phase_locked = true;
                }
            else
                {
                    d_flag_PLL_180_deg_phase_locked = false;
                }
            std::bitset<GPS_L5_CNAV_DATA_PAGE_BITS> raw_bits;
            // Expand packet bits to bitsets. Notice the reverse order of the bits sequence, required by the CNAV message decoder
            for (uint32_t i = 0; i < GPS_L5_CNAV_DATA_PAGE_BITS; i++)
                {
                    raw_bits[GPS_L5_CNAV_DATA_PAGE_BITS - 1 - i] = ((msg.raw_msg[i / 8] >> (7 - i % 8)) & 1U);
                }

            d_CNAV_Message.decode_page(raw_bits);

            // Push the new navigation data to the queues
            if (d_CNAV_Message.have_new_ephemeris() == true)
                {
                    // get ephemeris object for this SV
                    std::shared_ptr<Gps_CNAV_Ephemeris> tmp_obj = std::make_shared<Gps_CNAV_Ephemeris>(d_CNAV_Message.get_ephemeris());
                    std::cout << TEXT_MAGENTA << "New GPS L5 CNAV message received in channel " << d_channel << ": ephemeris from satellite " << d_satellite << TEXT_RESET << std::endl;
                    this->message_port_pub(pmt::mp("telemetry"), pmt::make_any(tmp_obj));
                }
            if (d_CNAV_Message.have_new_iono() == true)
                {
                    std::shared_ptr<Gps_CNAV_Iono> tmp_obj = std::make_shared<Gps_CNAV_Iono>(d_CNAV_Message.get_iono());
                    std::cout << TEXT_MAGENTA << "New GPS L5 CNAV message received in channel " << d_channel << ": iono model parameters from satellite " << d_satellite << TEXT_RESET << std::endl;
                    this->message_port_pub(pmt::mp("telemetry"), pmt::make_any(tmp_obj));
                }

            if (d_CNAV_Message.have_new_utc_model() == true)
                {
                    std::shared_ptr<Gps_CNAV_Utc_Model> tmp_obj = std::make_shared<Gps_CNAV_Utc_Model>(d_CNAV_Message.get_utc_model());
                    std::cout << TEXT_MAGENTA << "New GPS L5 CNAV message received in channel " << d_channel << ": UTC model parameters from satellite " << d_satellite << TEXT_RESET << std::endl;
                    this->message_port_pub(pmt::mp("telemetry"), pmt::make_any(tmp_obj));
                }

            // update TOW at the preamble instant
            d_TOW_at_Preamble_ms = msg.tow * 6000;

            // The time of the last input symbol can be computed from the message ToW and
            // delay by the formulae:
            // \code
            // symbolTime_ms = msg->tow * 6000 + *pdelay * 10 + (12 * 10); 12 symbols of the encoder's transitory

            // check TOW update consistency
            uint32_t last_d_TOW_at_current_symbol_ms = d_TOW_at_current_symbol_ms;
            d_TOW_at_current_symbol_ms = msg.tow * 6000 + (delay + 12) * GPS_L5I_SYMBOL_PERIOD_MS;
            if (last_d_TOW_at_current_symbol_ms != 0 and std::llabs(static_cast<int64_t>(d_TOW_at_current_symbol_ms) - static_cast<int64_t>(last_d_TOW_at_current_symbol_ms)) > static_cast<int64_t>(GPS_L5I_SYMBOL_PERIOD_MS))
                {
                    DLOG(INFO) << "Warning: GPS L5 TOW update in ch " << d_channel
                               << " does not match the TLM TOW counter " << static_cast<int64_t>(d_TOW_at_current_symbol_ms) - static_cast<int64_t>(last_d_TOW_at_current_symbol_ms) << " ms "
                               << " with delay: " << delay << " msg tow: " << msg.tow * 6000 << " ms \n";

                    d_TOW_at_current_symbol_ms = 0;
                    d_flag_valid_word = false;
                }
            else
                {
                    d_last_valid_preamble = d_sample_counter;
                    d_flag_valid_word = true;
                }
        }
    else
        {
            if (d_flag_valid_word)
                {
                    d_TOW_at_current_symbol_ms += GPS_L5I_SYMBOL_PERIOD_MS;
                    if (current_synchro_data.Flag_valid_symbol_output == false)
                        {
                            d_flag_valid_word = false;
                        }
                }
        }

    if (d_flag_valid_word == true)
        {
            if (d_flag_PLL_180_deg_phase_locked == true)
                {
                    // correct the accumulated phase for the Costas loop phase shift, if required
                    current_synchro_data.Carrier_phase_rads += GPS_L5_PI;
                }
            current_synchro_data.TOW_at_current_symbol_ms = d_TOW_at_current_symbol_ms;
            current_synchro_data.Flag_valid_word = d_flag_valid_word;

            if (d_dump == true)
                {
                    // MULTIPLEXED FILE RECORDING - Record results to file
                    try
                        {
                            double tmp_double;
                            uint64_t tmp_ulong_int;
                            tmp_double = static_cast<double>(d_TOW_at_current_symbol_ms) / 1000.0;
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_double), sizeof(double));
                            tmp_ulong_int = current_synchro_data.Tracking_sample_counter;
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_ulong_int), sizeof(uint64_t));
                            tmp_double = static_cast<double>(d_TOW_at_Preamble_ms) / 1000.0;
                            d_dump_file.write(reinterpret_cast<char *>(&tmp_double), sizeof(double));
                        }
                    catch (const std::ifstream::failure &e)
                        {
                            LOG(WARNING) << "Exception writing Telemetry GPS L5 dump file " << e.what();
                        }
                }

            // 3. Make the output (copy the object contents to the GNURadio reserved memory)
            out[0] = current_synchro_data;
            return 1;
        }
    return 0;
}
