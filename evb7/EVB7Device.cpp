//
// SoapySDR wrapper for the LMS7002M driver.
//
// Copyright (c) 2015-2015 Fairwaves, Inc.
// Copyright (c) 2015-2015 Rice University
// SPDX-License-Identifier: Apache-2.0
// http://www.apache.org/licenses/LICENSE-2.0
//

#include "EVB7Device.hpp"
#include <SoapySDR/Registry.hpp>

/***********************************************************************
 * Constructor
 **********************************************************************/
EVB7::EVB7(void):
    _regs(NULL),
    _spiHandle(NULL),
    _lms(NULL),
    _rx_data_dma(NULL),
    _rx_ctrl_dma(NULL),
    _tx_data_dma(NULL),
    _tx_stat_dma(NULL),
    _masterClockRate(1.0e6)
{
    SoapySDR::logf(SOAPY_SDR_INFO, "EVB7()");
    setvbuf(stdout, NULL, _IOLBF, 0);

    //map FPGA registers
    _regs = xumem_map_phys(FPGA_REGS, FPGA_REGS_SIZE);
    if (_regs == NULL)
    {
        throw std::runtime_error("EVB7 fail to map registers");
    }
    SoapySDR::logf(SOAPY_SDR_INFO, "Read sentinel 0x%x\n", xumem_read32(_regs, FPGA_REG_RD_SENTINEL));
    this->writeRegister(FPGA_REG_WR_TX_TEST, 0); //test off, normal tx from deframer

    //perform reset
    SET_EMIO_OUT_LVL(RESET_EMIO, 0);
    SET_EMIO_OUT_LVL(RESET_EMIO, 1);

    //setup spi
    _spiHandle = spidev_interface_open("/dev/spidev32766.0");
    if (_spiHandle == NULL) std::runtime_error("EVB7 fail to spidev_interface_open()");

    //setup LMS7002M
    _lms = LMS7002M_create(spidev_interface_transact, _spiHandle);
    if (_lms == NULL) std::runtime_error("EVB7 fail to LMS7002M_create()");
    LMS7002M_set_spi_mode(_lms, 4); //set 4-wire spi mode first
    LMS7002M_reset(_lms);

    //enable all ADCs and DACs
    LMS7002M_afe_enable(_lms, LMS_TX, LMS_CHA, true);
    LMS7002M_afe_enable(_lms, LMS_TX, LMS_CHB, true);
    LMS7002M_afe_enable(_lms, LMS_RX, LMS_CHA, true);
    LMS7002M_afe_enable(_lms, LMS_RX, LMS_CHB, true);

    //LMS7002M_load_ini(_lms, "/root/src/test2.ini");
    //LMS7002M_set_spi_mode(_lms, 4); //set 4-wire spi mode first

    //read info register
    LMS7002M_regs_spi_read(_lms, 0x002f);
    SoapySDR::logf(SOAPY_SDR_INFO, "rev 0x%x", LMS7002M_regs(_lms)->reg_0x002f_rev);
    SoapySDR::logf(SOAPY_SDR_INFO, "ver 0x%x", LMS7002M_regs(_lms)->reg_0x002f_ver);

    //turn the clocks on
    this->setMasterClockRate(61e6);

    //configure data port directions and data clock rates
    LMS7002M_configure_lml_port(_lms, LMS_PORT1, LMS_TX, 1);
    LMS7002M_configure_lml_port(_lms, LMS_PORT2, LMS_RX, 1);

    //external reset now that clocks are on
    this->writeRegister(FPGA_REG_WR_EXT_RST, 1);
    this->writeRegister(FPGA_REG_WR_EXT_RST, 0);

    //estimate the clock rates with readback registers
    uint32_t rxt0 = xumem_read32(_regs, FPGA_REG_RD_RX_CLKS);
    uint32_t txt0 = xumem_read32(_regs, FPGA_REG_RD_TX_CLKS);
    sleep(1);
    uint32_t rxt1 = xumem_read32(_regs, FPGA_REG_RD_RX_CLKS);
    uint32_t txt1 = xumem_read32(_regs, FPGA_REG_RD_TX_CLKS);
    SoapySDR::logf(SOAPY_SDR_INFO, "RX rate %f Mhz", (rxt1-rxt0)/1e6);
    SoapySDR::logf(SOAPY_SDR_INFO, "TX rate %f Mhz", (txt1-txt0)/1e6);

    //clear time
    this->setHardwareTime(0, "");
    /*
    long long t0 = this->getHardwareTime("");
    sleep(1);
    long long t1 = this->getHardwareTime("");
    SoapySDR::logf(SOAPY_SDR_INFO, "HW sec/PC sec %f", (t1-t0)/1e9);
    */

    //port output enables
    SET_EMIO_OUT_LVL(RXEN_EMIO, 1);
    SET_EMIO_OUT_LVL(TXEN_EMIO, 1);

    //setup dsp
    LMS7002M_rxtsp_init(_lms, LMS_CHAB);
    LMS7002M_txtsp_init(_lms, LMS_CHAB);

    //setup dma buffs
    _rx_data_dma = pzdud_create(RX_DMA_INDEX, PZDUD_S2MM);
    if (_rx_data_dma == NULL) throw std::runtime_error("EVB7 fail create rx data DMA");
    pzdud_reset(_rx_data_dma);

    _rx_ctrl_dma = pzdud_create(RX_DMA_INDEX, PZDUD_MM2S);
    if (_rx_ctrl_dma == NULL) throw std::runtime_error("EVB7 fail create rx ctrl DMA");
    pzdud_reset(_rx_ctrl_dma);

    _tx_data_dma = pzdud_create(TX_DMA_INDEX, PZDUD_MM2S);
    if (_tx_data_dma == NULL) throw std::runtime_error("EVB7 fail create tx data DMA");
    pzdud_reset(_tx_data_dma);

    _tx_stat_dma = pzdud_create(TX_DMA_INDEX, PZDUD_S2MM);
    if (_tx_stat_dma == NULL) throw std::runtime_error("EVB7 fail create tx stat DMA");
    pzdud_reset(_tx_stat_dma);

    SoapySDR::logf(SOAPY_SDR_INFO, "EVB7() setup OK");

    //try test
    /*
    this->writeRegister(FPGA_REG_WR_TX_TEST, 1); //test registers drive tx
    this->writeRegister(FPGA_REG_WR_TX_CHA, 0xAAAABBBB);
    this->writeRegister(FPGA_REG_WR_TX_CHB, 0xCCCCDDDD);
    LMS7002M_setup_digital_loopback(_lms);
    sleep(1);
    SoapySDR::logf(SOAPY_SDR_INFO, "FPGA_REG_RD_RX_CHA 0x%x", xumem_read32(_regs, FPGA_REG_RD_RX_CHA));
    SoapySDR::logf(SOAPY_SDR_INFO, "FPGA_REG_RD_RX_CHB 0x%x", xumem_read32(_regs, FPGA_REG_RD_RX_CHB));
    //*/

    LMS7002M_rxtsp_tsg_const(_lms, LMS_CHA, 1 << 14, 1 << 14);
    LMS7002M_rxtsp_tsg_const(_lms, LMS_CHB, 1 << 14, 1 << 14);
/*
    LMS7002M_rxtsp_tsg_tone(_lms, LMS_CHA);
    LMS7002M_rxtsp_tsg_tone(_lms, LMS_CHB);
    */
/*
    sleep(1);
    SoapySDR::logf(SOAPY_SDR_INFO, "FPGA_REG_RD_RX_CHA 0x%x", xumem_read32(_regs, FPGA_REG_RD_RX_CHA));
    SoapySDR::logf(SOAPY_SDR_INFO, "FPGA_REG_RD_RX_CHB 0x%x", xumem_read32(_regs, FPGA_REG_RD_RX_CHB));

    LMS7002M_dump_ini(_lms, "/tmp/regs.ini");
*/
    //some defaults to avoid throwing
    _cachedSampleRates[SOAPY_SDR_RX] = 1e6;
    _cachedSampleRates[SOAPY_SDR_TX] = 1e6;
    _cachedLOFrequencies[SOAPY_SDR_RX] = 1e9;
    _cachedLOFrequencies[SOAPY_SDR_TX] = 1e9;
    _cachedBBFrequencies[SOAPY_SDR_RX][0] = 0;
    _cachedBBFrequencies[SOAPY_SDR_TX][0] = 0;
    _cachedBBFrequencies[SOAPY_SDR_RX][1] = 0;
    _cachedBBFrequencies[SOAPY_SDR_TX][1] = 0;
}

EVB7::~EVB7(void)
{
    //power down and clean up
    LMS7002M_power_down(_lms);
    LMS7002M_destroy(_lms);

    //back to inputs
    CLEANUP_EMIO(RESET_EMIO);
    CLEANUP_EMIO(RXEN_EMIO);
    CLEANUP_EMIO(TXEN_EMIO);

    //dma cleanup
    pzdud_destroy(_rx_data_dma);
    pzdud_destroy(_rx_ctrl_dma);
    pzdud_destroy(_tx_data_dma);
    pzdud_destroy(_tx_stat_dma);

    //spi cleanup
    spidev_interface_close(_spiHandle);

    //register unmap
    xumem_unmap_phys(_regs, FPGA_REGS_SIZE);
}

/*******************************************************************
 * Frequency API
 ******************************************************************/
void EVB7::setFrequency(const int direction, const size_t channel, const double frequency, const SoapySDR::Kwargs &args)
{
    //the rf frequency as passed -n or specifically from args
    //double rfFreq = (args.count("RF") != 0)?atof(args.at("RF").c_str()):frequency;

    //optional LO offset from the args
    //double offset = (args.count("OFFSET") != 0)?atof(args.at("OFFSET").c_str()):0.0;

    //rfFreq += offset;

    //tune the LO
    //int ret = LMS7002M_set_lo_freq(_lms, (direction == SOAPY_SDR_RX)?LMS_RX:LMS_RX, _masterClockRate, frequency, &_cachedLOFrequencies[direction]);

    //for now, we just tune the cordics
    const double baseRate = this->getTSPRate(direction);
    if (direction == SOAPY_SDR_RX) LMS7002M_rxtsp_set_freq(_lms, (channel == 0)?LMS_CHA:LMS_CHB, frequency/baseRate);
    if (direction == SOAPY_SDR_TX) LMS7002M_txtsp_set_freq(_lms, (channel == 0)?LMS_CHA:LMS_CHB, frequency/baseRate);
    _cachedBBFrequencies[direction][channel] = frequency;
}

double EVB7::getFrequency(const int direction, const size_t channel) const
{
    return getFrequency(direction, channel, "BB") + getFrequency(direction, channel, "RF");
}

double EVB7::getFrequency(const int direction, const size_t channel, const std::string &name) const
{
    if (name == "BB") return _cachedBBFrequencies.at(direction).at(channel);
    if (name == "RF") return _cachedLOFrequencies.at(direction);
    return SoapySDR::Device::getFrequency(direction, channel, name);
}

std::vector<std::string> EVB7::listFrequencies(const int, const size_t) const
{
    std::vector<std::string> opts;
    opts.push_back("RF");
    opts.push_back("BB");
    return opts;
}

SoapySDR::RangeList EVB7::getFrequencyRange(const int, const size_t) const
{
    SoapySDR::RangeList ranges;
    ranges.push_back(SoapySDR::Range(100e3, 3.8e9));
    return ranges;
}
/*******************************************************************
 * Sample Rate API
 ******************************************************************/
void EVB7::setSampleRate(const int direction, const size_t, const double rate)
{
    const double baseRate = this->getTSPRate(direction);
    const double factor = baseRate/rate;
    SoapySDR::logf(SOAPY_SDR_TRACE, "setSampleRate %f MHz, baseRate %f MHz, factor %f", rate/1e6, baseRate/1e6, factor);
    if (factor < 2.0) throw std::runtime_error("EVB7::setSampleRate() -- rate too high");
    const int intFactor = int(factor + 0.5);
    if (intFactor > 32) throw std::runtime_error("EVB7::setSampleRate() -- rate too low");

    if (std::abs(factor-intFactor) > 0.01) SoapySDR::logf(SOAPY_SDR_WARNING,
        "EVB7::setSampleRate(): not a power of two factor: TSP Rate = %f MHZ, Requested rate = %f MHz", baseRate/1e6, rate/1e6);

    //apply the settings, both the interp/decim has to be matched with the lml interface divider
    //the lml interface needs a clock rate 2x the sample rate for DDR TRX IQ mode
    if (direction == SOAPY_SDR_RX)
    {
        LMS7002M_rxtsp_set_decim(_lms, LMS_CHAB, intFactor);
        LMS7002M_configure_lml_port(_lms, LMS_PORT2, LMS_RX, intFactor/2);
    }
    if (direction == SOAPY_SDR_TX)
    {
        LMS7002M_txtsp_set_interp(_lms, LMS_CHAB, intFactor);
        LMS7002M_configure_lml_port(_lms, LMS_PORT1, LMS_TX, intFactor/2);
    }

    _cachedSampleRates[direction] = baseRate/intFactor;
}

double EVB7::getSampleRate(const int direction, const size_t) const
{
    return _cachedSampleRates.at(direction);
}

std::vector<double> EVB7::listSampleRates(const int direction, const size_t) const
{
    const double baseRate = this->getTSPRate(direction);
    std::vector<double> rates;
    for (int i = 5; i >= 0; i--)
    {
        rates.push_back(baseRate/(1 << i));
    }
    return rates;
}

/***********************************************************************
 * Find available devices
 **********************************************************************/
std::vector<SoapySDR::Kwargs> findEVB7(const SoapySDR::Kwargs &args)
{
    //always discovery "args" -- the sdr is the board itself
    std::vector<SoapySDR::Kwargs> discovered;
    discovered.push_back(args);
    return discovered;
}

/***********************************************************************
 * Make device instance
 **********************************************************************/
SoapySDR::Device *makeEVB7(const SoapySDR::Kwargs &)
{
    //ignore the args because again, the sdr is the board
    return new EVB7();
}

/***********************************************************************
 * Registration
 **********************************************************************/
static SoapySDR::Registry registerEVB7("evb7", &findEVB7, &makeEVB7, SOAPY_SDR_ABI_VERSION);