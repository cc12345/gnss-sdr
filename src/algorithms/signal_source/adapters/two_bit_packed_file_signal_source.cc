/*!
 * \file two_bit_packed_file_signal_source.cc
 * \brief Interface of a class that reads signals samples from a file. Each
 * sample is two bits, which are packed into bytes or shorts.
 *
 * \author Cillian O'Driscoll, 2015 cillian.odriscoll (at) gmail.com
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

#include "two_bit_packed_file_signal_source.h"
#include "configuration_interface.h"
#include "gnss_sdr_flags.h"
#include "gnss_sdr_valve.h"
#include <glog/logging.h>
#include <gnuradio/blocks/char_to_float.h>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <utility>


TwoBitPackedFileSignalSource::TwoBitPackedFileSignalSource(
    const ConfigurationInterface* configuration,
    const std::string& role,
    unsigned int in_streams,
    unsigned int out_streams,
    Concurrent_Queue<pmt::pmt_t>* queue) : role_(role),
                                           in_streams_(in_streams),
                                           out_streams_(out_streams)
{
    std::string default_filename = "../data/my_capture.dat";
    std::string default_item_type = "byte";
    std::string default_dump_filename = "../data/my_capture_dump.dat";
    std::string default_sample_type = "real";
    double default_seconds_to_skip = 0.0;

    samples_ = configuration->property(role + ".samples", static_cast<uint64_t>(0));
    sampling_frequency_ = configuration->property(role + ".sampling_frequency", static_cast<int64_t>(0));
    filename_ = configuration->property(role + ".filename", default_filename);

    // override value with commandline flag, if present
    if (FLAGS_signal_source != "-")
        {
            filename_ = FLAGS_signal_source;
        }
    if (FLAGS_s != "-")
        {
            filename_ = FLAGS_s;
        }

    item_type_ = configuration->property(role + ".item_type", default_item_type);
    big_endian_items_ = configuration->property(role + ".big_endian_items", true);
    big_endian_bytes_ = configuration->property(role + ".big_endian_bytes", false);
    sample_type_ = configuration->property(role + ".sample_type", default_sample_type);  // options: "real", "iq", "qi"
    repeat_ = configuration->property(role + ".repeat", false);
    dump_ = configuration->property(role + ".dump", false);
    dump_filename_ = configuration->property(role + ".dump_filename", default_dump_filename);
    enable_throttle_control_ = configuration->property(role + ".enable_throttle_control", false);
    double seconds_to_skip = configuration->property(role + ".seconds_to_skip", default_seconds_to_skip);
    int64_t bytes_to_skip = 0;

    if (item_type_ == "byte")
        {
            item_size_ = sizeof(char);
        }
    else if (item_type_ == "short")
        {
            // If we have shorts stored in little endian format, might as
            // well read them in as bytes.
            if (big_endian_items_)
                {
                    item_size_ = sizeof(int16_t);
                }
            else
                {
                    item_size_ = sizeof(char);
                }
        }
    else
        {
            LOG(WARNING) << item_type_ << " unrecognized item type. Using byte.";
            item_size_ = sizeof(char);
        }

    if (sample_type_ == "real")
        {
            is_complex_ = false;
        }
    else if (sample_type_ == "iq")
        {
            is_complex_ = true;
            reverse_interleaving_ = false;
        }
    else if (sample_type_ == "qi")
        {
            is_complex_ = true;
            reverse_interleaving_ = true;
        }
    else
        {
            LOG(WARNING) << sample_type_ << " unrecognized sample type. Assuming: "
                         << (is_complex_ ? (reverse_interleaving_ ? "qi" : "iq") : "real");
        }
    try
        {
            file_source_ = gr::blocks::file_source::make(item_size_, filename_.c_str(), repeat_);

            if (seconds_to_skip > 0)
                {
                    bytes_to_skip = static_cast<int64_t>(
                        seconds_to_skip * sampling_frequency_ / 4);
                    if (is_complex_)
                        {
                            bytes_to_skip <<= 1;
                        }
                    file_source_->seek(bytes_to_skip, SEEK_SET);
                }

            unpack_samples_ = make_unpack_2bit_samples(big_endian_bytes_,
                item_size_, big_endian_items_, reverse_interleaving_);
            if (is_complex_)
                {
                    char_to_float_ =
                        gr::blocks::interleaved_char_to_complex::make(false);
                }
            else
                {
                    char_to_float_ =
                        gr::blocks::char_to_float::make();
                }
        }
    catch (const std::exception& e)
        {
            std::cerr
                << "The receiver was configured to work with a file signal source "
                << std::endl
                << "but the specified file is unreachable by GNSS-SDR."
                << std::endl
                << "Please modify your configuration file"
                << std::endl
                << "and point SignalSource.filename to a valid raw data file. Then:"
                << std::endl
                << "$ gnss-sdr --config_file=/path/to/my_GNSS_SDR_configuration.conf"
                << std::endl
                << "Examples of configuration files available at:"
                << std::endl
                << GNSSSDR_INSTALL_DIR "/share/gnss-sdr/conf/"
                << std::endl;

            LOG(WARNING) << "file_signal_source: Unable to open the samples file "
                         << filename_.c_str() << ", exiting the program.";
            throw(e);
        }

    DLOG(INFO) << "file_source(" << file_source_->unique_id() << ")";

    size_t output_item_size = (is_complex_ ? sizeof(gr_complex) : sizeof(float));

    if (samples_ == 0)  // read all file
        {
            /*!
             * BUG workaround: The GNU Radio file source does not stop the receiver after reaching the End of File.
             * A possible solution is to compute the file length in samples using file size, excluding the last 2 milliseconds, and enable always the
             * valve block
             */
            std::ifstream file(filename_.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
            std::ifstream::pos_type size;

            if (file.is_open())
                {
                    size = file.tellg();
                    samples_ = floor(static_cast<double>(size) * (is_complex_ ? 2.0 : 4.0));
                    LOG(INFO) << "Total samples in the file= " << samples_;  // 4 samples per byte
                    samples_ -= bytes_to_skip;

                    // Also skip the last two milliseconds:
                    samples_ -= ceil(0.002 * sampling_frequency_ / (is_complex_ ? 2.0 : 4.0));
                }
            else
                {
                    std::cout << "file_signal_source: Unable to open the samples file " << filename_.c_str() << std::endl;
                    LOG(ERROR) << "file_signal_source: Unable to open the samples file " << filename_.c_str();
                }
            std::streamsize ss = std::cout.precision();
            std::cout << std::setprecision(16);
            std::cout << "Processing file " << filename_ << ", which contains " << size << " [bytes]" << std::endl;
            std::cout.precision(ss);
        }

    CHECK(samples_ > 0) << "File does not contain enough samples to process.";
    double signal_duration_s;
    signal_duration_s = static_cast<double>(samples_) * (1 / static_cast<double>(sampling_frequency_));
    LOG(INFO) << "Total number samples to be processed= " << samples_ << " GNSS signal duration= " << signal_duration_s << " [s]";
    std::cout << "GNSS signal recorded time to be processed: " << signal_duration_s << " [s]" << std::endl;

    valve_ = gnss_sdr_make_valve(output_item_size, samples_, queue);
    DLOG(INFO) << "valve(" << valve_->unique_id() << ")";

    if (dump_)
        {
            // sink_ = gr_make_file_sink(item_size_, dump_filename_.c_str());
            sink_ = gr::blocks::file_sink::make(output_item_size, dump_filename_.c_str());
            DLOG(INFO) << "file_sink(" << sink_->unique_id() << ")";
        }

    if (enable_throttle_control_)
        {
            throttle_ = gr::blocks::throttle::make(output_item_size, sampling_frequency_);
        }
    DLOG(INFO) << "File source filename " << filename_;
    DLOG(INFO) << "Samples " << samples_;
    DLOG(INFO) << "Sampling frequency " << sampling_frequency_;
    DLOG(INFO) << "Item type " << item_type_;
    DLOG(INFO) << "Item size " << item_size_;
    DLOG(INFO) << "Repeat " << repeat_;
    DLOG(INFO) << "Dump " << dump_;
    DLOG(INFO) << "Dump filename " << dump_filename_;
    if (in_streams_ > 0)
        {
            LOG(ERROR) << "A signal source does not have an input stream";
        }
    if (out_streams_ > 1)
        {
            LOG(ERROR) << "This implementation only supports one output stream";
        }
}


void TwoBitPackedFileSignalSource::connect(gr::top_block_sptr top_block)
{
    gr::basic_block_sptr left_block = file_source_;
    gr::basic_block_sptr right_block = unpack_samples_;

    top_block->connect(file_source_, 0, unpack_samples_, 0);
    left_block = right_block;

    DLOG(INFO) << "connected file source to unpack samples";
    right_block = char_to_float_;
    top_block->connect(left_block, 0, right_block, 0);
    left_block = right_block;
    DLOG(INFO) << "connected unpack samples to char to float";

    if (enable_throttle_control_)
        {
            right_block = throttle_;
            top_block->connect(left_block, 0, right_block, 0);
            left_block = right_block;
            DLOG(INFO) << " connected to throttle";
        }

    top_block->connect(left_block, 0, valve_, 0);
    DLOG(INFO) << "connected to valve";
    if (dump_)
        {
            top_block->connect(valve_, 0, sink_, 0);
            DLOG(INFO) << "connected valve to file sink";
        }
}


void TwoBitPackedFileSignalSource::disconnect(gr::top_block_sptr top_block)
{
    gr::basic_block_sptr left_block = file_source_;
    gr::basic_block_sptr right_block = unpack_samples_;

    top_block->disconnect(file_source_, 0, unpack_samples_, 0);
    left_block = right_block;

    DLOG(INFO) << "disconnected file source to unpack samples";
    right_block = char_to_float_;
    top_block->disconnect(left_block, 0, right_block, 0);
    left_block = right_block;
    DLOG(INFO) << "disconnected unpack samples to char to float";

    if (enable_throttle_control_)
        {
            right_block = throttle_;
            top_block->disconnect(left_block, 0, right_block, 0);
            left_block = right_block;
            DLOG(INFO) << " disconnected to throttle";
        }

    top_block->disconnect(left_block, 0, valve_, 0);
    DLOG(INFO) << "disconnected to valve";
    if (dump_)
        {
            top_block->disconnect(valve_, 0, sink_, 0);
            DLOG(INFO) << "disconnected valve to file sink";
        }
}


gr::basic_block_sptr TwoBitPackedFileSignalSource::get_left_block()
{
    LOG(WARNING) << "Left block of a signal source should not be retrieved";
    // return gr_block_sptr();
    return gr::blocks::file_source::sptr();
}


gr::basic_block_sptr TwoBitPackedFileSignalSource::get_right_block()
{
    return valve_;
}
