/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2021  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include <ell/ell.h>

#include "ell/useful.h"

#include "band.h"

void band_free(struct band *band)
{
	l_free(band);
}

/*
 * Base RSSI values for 20MHz (both HT and VHT) channel. These values can be
 * used to calculate the minimum RSSI values for all other channel widths. HT
 * MCS indexes are grouped into ranges of 8 (per spatial stream) where VHT are
 * grouped in chunks of 10. This just means HT will not use the last two
 * index's of this array.
 */
static const int32_t ht_vht_base_rssi[] = {
	-82, -79, -77, -74, -70, -66, -65, -64, -59, -57
};

/*
 * Data Rate for HT/VHT is obtained according to this formula:
 * Nsd * Nbpscs * R * Nss / (Tdft + Tgi)
 *
 * Where Nsd is [52, 108, 234, 468] for 20/40/80/160 Mhz respectively
 * Nbpscs is [1, 2, 4, 6, 8] for BPSK/QPSK/16QAM/64QAM/256QAM
 * R is [1/2, 2/3, 3/4, 5/6] depending on the MCS index
 * Nss is the number of spatial streams
 * Tdft = 3.2 us
 * Tgi = Long/Short GI of 0.8/0.4 us
 *
 * Short GI rate can be easily obtained by multiplying by (10 / 9)
 *
 * The table was pre-computed using the following python snippet:
 * rfactors = [ 1/2, 1/2, 3/4, 1/2, 3/4, 2/3, 3/4, 5/6, 3/4, 5/6 ]
 * nbpscs = [1, 2, 2, 4, 4, 6, 6, 6, 8, 8 ]
 * nsds = [52, 108, 234, 468]
 *
 * for nsd in nsds:
 * 	rates = []
 * 	for i in xrange(0, 10):
 * 		data_rate = (nsd * rfactors[i] * nbpscs[i]) / 0.004
 * 		rates.append(int(data_rate) * 1000)
 * 	print('rates for nsd: ' + nsd + ': ' + rates)
 */

static const uint64_t ht_vht_rates[4][10] = {
	[OFDM_CHANNEL_WIDTH_20MHZ] = {
		6500000ULL, 13000000ULL, 19500000ULL, 26000000ULL,
		39000000ULL, 52000000ULL, 58500000ULL, 65000000ULL,
		78000000ULL, 86666000ULL },
	[OFDM_CHANNEL_WIDTH_40MHZ] = {
		13500000ULL, 27000000ULL, 40500000ULL, 54000000ULL,
		81000000ULL, 108000000ULL, 121500000ULL, 135000000ULL,
		162000000ULL, 180000000ULL, },
	[OFDM_CHANNEL_WIDTH_80MHZ] = {
		29250000ULL, 58500000ULL, 87750000ULL, 117000000ULL,
		175500000ULL, 234000000ULL, 263250000ULL, 292500000ULL,
		351000000ULL, 390000000ULL, },
	[OFDM_CHANNEL_WIDTH_160MHZ] = {
		58500000ULL, 117000000ULL, 175500000ULL, 234000000ULL,
		351000000ULL, 468000000ULL, 526500000ULL, 585000000ULL,
		702000000ULL, 780000000ULL,
	}
};

/*
 * Both HT and VHT rates are calculated in the same fashion. The only difference
 * is a relative MCS index is used for HT since, for each NSS, the formula
 * is the same with relative index's. This is why this is called with index % 8
 * for HT, but not VHT.
 */
bool band_ofdm_rate(uint8_t index, enum ofdm_channel_width width,
			int32_t rssi, uint8_t nss, bool sgi,
			uint64_t *data_rate)
{
	uint64_t rate;
	int32_t width_adjust = width * 3;

	if (rssi < ht_vht_base_rssi[index] + width_adjust)
		return false;

	rate = ht_vht_rates[width][index];

	if (sgi)
		rate = rate / 9 * 10;

	rate *= nss;

	*data_rate = rate;
	return true;
}

static bool find_best_mcs_vht(uint8_t max_index, enum ofdm_channel_width width,
				int32_t rssi, uint8_t nss, bool sgi,
				uint64_t *out_data_rate)
{
	int i;

	/*
	 * Iterate over all available MCS indexes to find the best one
	 * we can use.  Note that band_ofdm_rate() will return false if a
	 * given combination cannot be used due to rssi being too low.
	 *
	 * Also, Certain MCS/Width/NSS combinations are not valid,
	 * refer to IEEE 802.11-2016 Section 21.5 for more details
	 */

	for (i = max_index; i >= 0; i--)
		if (band_ofdm_rate(i, width, rssi,
						nss, sgi, out_data_rate))
			return true;

	return false;
}

/*
 * IEEE 802.11 - Table 9-250
 *
 * For simplicity, we are ignoring the Extended BSS BW support, per NOTE 11:
 *
 * NOTE 11-A receiving STA in which dot11VHTExtendedNSSCapable is false will
 * ignore the Extended NSS BW Support subfield and effectively evaluate this
 * table only at the entries where Extended NSS BW Support is 0.
 *
 * This also allows us to group the 160/80+80 widths together, since they are
 * the same when Extended NSS BW is zero.
 */
int band_estimate_vht_rx_rate(const struct band *band,
				const uint8_t *vhtc, const uint8_t *vhto,
				const uint8_t *htc, const uint8_t *hto,
				int32_t rssi, uint64_t *out_data_rate)
{
	uint32_t nss = 0;
	uint32_t max_mcs = 7; /* MCS 0-7 for NSS:1 is always supported */
	const uint8_t *rx_mcs_map;
	const uint8_t *tx_mcs_map;
	int bitoffset;
	uint8_t chan_width;
	uint8_t channel_offset;
	bool sgi;

	if (!band->vht_supported || !band->ht_supported)
		return -ENOTSUP;

	if (!vhtc || !vhto || !htc || !hto)
		return -ENOTSUP;

	if (vhto[2] > 3)
		return -EBADMSG;

	/*
	 * Find the highest NSS/MCS index combination.  Since this is used by
	 * STAs, we try to estimate our 'download' speed from the AP/peer.
	 * Hence we look at the TX MCS map of the peer and our own RX MCS map
	 * to find an overlapping combination that works
	 */
	rx_mcs_map = band->vht_mcs_set;
	tx_mcs_map = vhtc + 2 + 8;

	for (bitoffset = 14; bitoffset >= 0; bitoffset -= 2) {
		uint8_t rx_val = bit_field(rx_mcs_map[bitoffset / 8],
							bitoffset % 8, 2);
		uint8_t tx_val = bit_field(tx_mcs_map[bitoffset / 8],
							bitoffset % 8, 2);

		/*
		 * 0 indicates support for MCS 0-7
		 * 1 indicates support for MCS 0-8
		 * 2 indicates support for MCS 0-9
		 * 3 indicates no support
		 */

		if (rx_val == 3 || tx_val == 3)
			continue;

		/* 7 + rx_val/tx_val gives us the maximum mcs index */
		max_mcs = minsize(rx_val, tx_val) + 7;
		nss = bitoffset / 2 + 1;
		break;
	}

	if (!nss)
		return -EBADMSG;

	/*
	 * There is no way to know whether a peer would send us packets using
	 * the short guard interval (SGI.)  SGI capability is only used to
	 * indicate whether the peer can accept packets that we send this way.
	 * Here we make the assumption that if the peer has the capability to
	 * accept packets using SGI and we have the capability to do so, then
	 * SGI will be used
	 *
	 * Also, we assume that the highest bandwidth will result in the
	 * highest rate for any given rssi.  Even accounting for invalid
	 * MCS/Width/NSS combinations, the higher channel width results
	 * in better data rate at [mcs index - 2] compared to [mcs index] of
	 * a next lower bandwidth.
	 */

	/* See if 160 Mhz operation is available */
	chan_width = bit_field(band->vht_capabilities[0], 2, 2);
	if (chan_width != 1 && chan_width != 2)
		goto try_vht80;

	/*
	 * Channel Width is set to 2 or 3, or 1 and
	 * channel center frequency segment 1 is non-zero
	 */
	if (vhto[2] == 2 || vhto[2] == 3 || (vhto[2] == 1 && vhto[4])) {
		sgi = test_bit(band->vht_capabilities, 6) &&
						test_bit(vhtc + 2, 6);

		if (find_best_mcs_vht(max_mcs, OFDM_CHANNEL_WIDTH_160MHZ,
					rssi, nss, sgi, out_data_rate))
			return 0;
	}

try_vht80:
	if (vhto[2] == 1) {
		sgi = test_bit(band->vht_capabilities, 5) &&
						test_bit(vhtc + 2, 5);

		if (find_best_mcs_vht(max_mcs, OFDM_CHANNEL_WIDTH_80MHZ,
					rssi, nss, sgi, out_data_rate))
			return 0;
	} /* Otherwise, assume 20/40 Operation */

	channel_offset = bit_field(hto[3], 0, 2);

	/* Test for 40 Mhz operation */
	if (test_bit(hto + 3, 2) &&
			(channel_offset == 1 || channel_offset == 3)) {
		sgi = test_bit(band->ht_capabilities, 6) &&
						test_bit(htc + 2, 6);

		if (find_best_mcs_vht(max_mcs, OFDM_CHANNEL_WIDTH_40MHZ,
					rssi, nss, sgi, out_data_rate))
			return 0;
	}

	sgi = test_bit(band->ht_capabilities, 5) && test_bit(htc + 2, 5);

	if (find_best_mcs_vht(max_mcs, OFDM_CHANNEL_WIDTH_20MHZ,
				rssi, nss, sgi, out_data_rate))
		return 0;

	return -EINVAL;
}