//==========================================================================
// Name:            filter_frequency.h
// Purpose:         Amateur band enumeration and frequency-to-band lookup
// Created:         April 2026
// Authors:         Mooneer Salem, Barry Jackson
//
// License:
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.1,
//  as published by the Free Software Foundation.  This program is
//  distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
//  License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, see <http://www.gnu.org/licenses/>.
//
//==========================================================================

#ifndef FILTER_FREQUENCY_H
#define FILTER_FREQUENCY_H

#include <cstdint>

enum FilterFrequency
{
    BAND_ALL,
    BAND_160M,
    BAND_80M,
    BAND_60M,
    BAND_40M,
    BAND_30M,
    BAND_20M,
    BAND_17M,
    BAND_15M,
    BAND_12M,
    BAND_10M,
    BAND_VHF_UHF,
    BAND_OTHER,
};

// Returns the amateur band containing the given frequency (in Hz), or
// BAND_OTHER if it falls outside all of them.
inline FilterFrequency getFilterForFrequency(uint64_t freq)
{
    if (freq >= 1800000 && freq <= 2000000) return BAND_160M;
    if (freq >= 3500000 && freq <= 4000000) return BAND_80M;
    if (freq >= 5250000 && freq <= 5450000) return BAND_60M;
    if (freq >= 7000000 && freq <= 7300000) return BAND_40M;
    if (freq >= 10100000 && freq <= 10150000) return BAND_30M;
    if (freq >= 14000000 && freq <= 14350000) return BAND_20M;
    if (freq >= 18068000 && freq <= 18168000) return BAND_17M;
    if (freq >= 21000000 && freq <= 21450000) return BAND_15M;
    if (freq >= 24890000 && freq <= 24990000) return BAND_12M;
    if (freq >= 28000000 && freq <= 29700000) return BAND_10M;
    if (freq >= 50000000) return BAND_VHF_UHF;
    return BAND_OTHER;
}

#endif // FILTER_FREQUENCY_H
