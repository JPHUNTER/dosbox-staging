/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Copyright (C) 2020-2022  The DOSBox Staging Team
 *  Copyright (C) 2002-2019  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

// NOTE: a lot of this code assumes that the callback is called every emulated
// millisecond

#include "dosbox.h"

// #define SPKR_DEBUGGING
// #define REFERENCE
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

#include "mixer.h"
#include "timer.h"
#include "setup.h"
#include "support.h"
#include "pic.h"

// constants
constexpr auto SPKR_ENTRIES = 1024;

constexpr int16_t SPKR_POSITIVE_LEVEL = 20000;
constexpr int16_t SPKR_NEGATIVE_LEVEL = -SPKR_POSITIVE_LEVEL;

constexpr auto SPKR_FILTER_QUALITY = 100;
constexpr auto SPKR_OVERSAMPLING = 32;
constexpr auto SPKR_CUTOFF_MARGIN = 0.2f; // must be greater than 0.0f
constexpr auto SPKR_FILTER_WIDTH = (SPKR_FILTER_QUALITY * SPKR_OVERSAMPLING);
constexpr auto SPKR_HIGHPASS = 0.999f; // Should be selected based on
                                       // sampling rate
constexpr auto ms_per_pit_tick = 1000.0f / PIT_TICK_RATE;
constexpr auto pi_f = static_cast<float>(M_PI);

struct DelayEntry {
	float index = 0.0f;
	int16_t output_level = SPKR_NEGATIVE_LEVEL;
};

static struct {
	mixer_channel_t chan = nullptr;
	std::vector<float> output_buffer = {};
	std::vector<float> sampled_impulse = {};

	uint8_t pit_mode = 0;
	int rate = 0;
	float rate_as_float = 0.0f;
	float rate_per_ms = 0.0f;

	bool pit_output_enabled = false;
	bool pit_clock_gate_enabled = false;
	int16_t pit_output_level = SPKR_NEGATIVE_LEVEL;
	float pit_new_max = 0.0f;
	float pit_new_half = 0.0f;
	float pit_max = 0.0f;
	float pit_half = 0.0f;
	float pit_index = 0.0f;
	bool pit_mode1_waiting_for_counter = false;
	bool pit_mode1_waiting_for_trigger = false;
	float pit_mode1_pending_max = 0.0f;

	bool pit_mode3_counting = false;
	float volwant, volcur = 0.0f;
	// Bitu last_ticks;
	float last_index = 0.0f;
	int minimum_counter = 0;
	DelayEntry entries[SPKR_ENTRIES] = {};
	int used = 0;
} spkr = {};

inline static void AddDelayEntry(const float index, const int16_t new_output_level)
{
#ifdef SPKR_DEBUGGING
	if (index < 0 || index > 1) {
		LOG_MSG("AddDelayEntry: index out of range %f at %f",
		        index,
		        PIC_FullIndex());
	}
#endif
	static auto previous_output_level = SPKR_NEGATIVE_LEVEL;
	if (new_output_level == previous_output_level) {
		return;
	}
	previous_output_level = new_output_level;
	if (spkr.used == SPKR_ENTRIES) {
		return;
	}
	spkr.entries[spkr.used].index = index;
	spkr.entries[spkr.used].output_level = new_output_level;
	spkr.used++;
}

inline static void AddPITOutput(const float index)
{
	if (spkr.pit_output_enabled) {
		AddDelayEntry(index, spkr.pit_output_level);
	}
}

static void ForwardPIT(const float newindex)
{
#ifdef SPKR_DEBUGGING
	if (newindex < 0 || newindex > 1) {
		LOG_MSG("ForwardPIT: index out of range %f at %f",
		        newindex,
		        PIC_FullIndex());
	}
#endif
	float passed = (newindex - spkr.last_index);
	float delay_base = spkr.last_index;
	spkr.last_index = newindex;
	switch (spkr.pit_mode) {
	case 6: // dummy
		return;
	case 0:
		if (spkr.pit_index >= spkr.pit_max) {
			return; // counter reached zero before previous call so
			        // do nothing
		}
		spkr.pit_index += passed;
		if (spkr.pit_index >= spkr.pit_max) {
			// counter reached zero between previous and this call
			const float delay = delay_base + spkr.pit_max -
			                    spkr.pit_index + passed;
			spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
			AddPITOutput(delay);
		}
		return;
	case 1:
		if (spkr.pit_mode1_waiting_for_counter) {
			// assert output level is high
			return; // counter not written yet
		}
		if (spkr.pit_mode1_waiting_for_trigger) {
			// assert output level is high
			return; // no pulse yet
		}
		if (spkr.pit_index >= spkr.pit_max) {
			return; // counter reached zero before previous call so
			        // do nothing
		}
		spkr.pit_index += passed;
		if (spkr.pit_index >= spkr.pit_max) {
			// counter reached zero between previous and this call
			const float delay = delay_base + spkr.pit_max -
			                    spkr.pit_index + passed;
			spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
			AddPITOutput(delay);
			// finished with this pulse
			spkr.pit_mode1_waiting_for_trigger = 1;
		}
		return;
	case 2:
		while (passed > 0) {
			/* passed the initial low cycle? */
			if (spkr.pit_index >= spkr.pit_half) {
				/* Start a new low cycle */
				if ((spkr.pit_index + passed) >= spkr.pit_max) {
					const float delay = spkr.pit_max -
					                    spkr.pit_index;
					delay_base += delay;
					passed -= delay;
					spkr.pit_output_level = SPKR_NEGATIVE_LEVEL;
					AddPITOutput(delay_base);
					spkr.pit_index = 0;
				} else {
					spkr.pit_index += passed;
					return;
				}
			} else {
				if ((spkr.pit_index + passed) >= spkr.pit_half) {
					const float delay = spkr.pit_half -
					                    spkr.pit_index;
					delay_base += delay;
					passed -= delay;
					spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
					AddPITOutput(delay_base);
					spkr.pit_index = spkr.pit_half;
				} else {
					spkr.pit_index += passed;
					return;
				}
			}
		}
		break;
		// END CASE 2
	case 3:
		if (!spkr.pit_mode3_counting)
			break;
		while (passed > 0) {
			/* Determine where in the wave we're located */
			if (spkr.pit_index >= spkr.pit_half) {
				if ((spkr.pit_index + passed) >= spkr.pit_max) {
					const float delay = spkr.pit_max -
					                    spkr.pit_index;
					delay_base += delay;
					passed -= delay;
					spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
					AddPITOutput(delay_base);
					spkr.pit_index = 0;
					/* Load the new count */
					spkr.pit_half = spkr.pit_new_half;
					spkr.pit_max = spkr.pit_new_max;
				} else {
					spkr.pit_index += passed;
					return;
				}
			} else {
				if ((spkr.pit_index + passed) >= spkr.pit_half) {
					const float delay = spkr.pit_half -
					                    spkr.pit_index;
					delay_base += delay;
					passed -= delay;
					spkr.pit_output_level = SPKR_NEGATIVE_LEVEL;
					AddPITOutput(delay_base);
					spkr.pit_index = spkr.pit_half;
					/* Load the new count */
					spkr.pit_half = spkr.pit_new_half;
					spkr.pit_max = spkr.pit_new_max;
				} else {
					spkr.pit_index += passed;
					return;
				}
			}
		}
		break;
		// END CASE 3
	case 4:
		if (spkr.pit_index < spkr.pit_max) {
			/* Check if we're gonna pass the end this block */
			if (spkr.pit_index + passed >= spkr.pit_max) {
				const float delay = spkr.pit_max - spkr.pit_index;
				delay_base += delay;
				passed -= delay;
				spkr.pit_output_level = SPKR_NEGATIVE_LEVEL;
				AddPITOutput(delay_base); // No new events
				                          // unless reprogrammed
				spkr.pit_index = spkr.pit_max;
			} else
				spkr.pit_index += passed;
		}
		break;
		// END CASE 4
	}
}

void PCSPEAKER_SetPITControl(const uint8_t mode)
{
	const auto newindex = static_cast<float>(PIC_TickIndex());
	ForwardPIT(newindex);
#ifdef SPKR_DEBUGGING
	LOG_INFO("PCSPEAKER: %f pit command: %u", PIC_FullIndex(), mode);
#endif
	// TODO: implement all modes
	switch (mode) {
	case 1:
		spkr.pit_mode = 1;
		spkr.pit_mode1_waiting_for_counter = 1;
		spkr.pit_mode1_waiting_for_trigger = 0;
		spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
		break;
	case 3:
		spkr.pit_mode = 3;
		spkr.pit_mode3_counting = 0;
		spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
		break;
	default: return;
	}
	AddPITOutput(newindex);
}

void PCSPEAKER_SetCounter(const int cntr, const uint8_t mode)
{
#ifdef SPKR_DEBUGGING
	LOG_INFO("PCSPEAKER: %f counter: %u, mode: %u", PIC_FullIndex(), cntr, mode);
#endif
	// if (!spkr.last_ticks) {
	//	if(spkr.chan) spkr.chan->Enable(true);
	//	spkr.last_index=0;
	// }
	// spkr.last_ticks=PIC_Ticks;
	const auto newindex = static_cast<float>(PIC_TickIndex());
	const auto duration_of_count_ms = ms_per_pit_tick * static_cast<float>(cntr);
	ForwardPIT(newindex);
	switch (mode) {
	case 0: /* Mode 0 one shot, used with "realsound" (PWM) */
		// if (cntr>80) {
		//	cntr=80;
		// }
		// spkr.pit_output_level=((float)cntr-40)*(SPKR_VOLUME/40.0f);
		spkr.pit_output_level = SPKR_NEGATIVE_LEVEL;
		spkr.pit_index = 0;
		spkr.pit_max = duration_of_count_ms;
		AddPITOutput(newindex);
		break;
	case 1: // retriggerable one-shot, used by Star Control 1
		spkr.pit_mode1_pending_max = duration_of_count_ms;
		if (spkr.pit_mode1_waiting_for_counter) {
			// assert output level is high
			spkr.pit_mode1_waiting_for_counter = 0;
			spkr.pit_mode1_waiting_for_trigger = 1;
		}
		break;
	case 2: /* Single cycle low, rest low high generator */
		spkr.pit_index = 0;
		spkr.pit_output_level = SPKR_NEGATIVE_LEVEL;
		AddPITOutput(newindex);
		spkr.pit_half = ms_per_pit_tick;
		spkr.pit_max = duration_of_count_ms;
		break;
	case 3: /* Square wave generator */
		if (cntr < spkr.minimum_counter) {
			// #ifdef SPKR_DEBUGGING
			//			LOG_MSG(
			//				"SetCounter: too high
			// frequency %u (cntr %u) at %f",
			// PIT_TICK_RATE/cntr, 				cntr,
			// PIC_FullIndex()); #endif
			//  hack to save CPU cycles
			// cntr = spkr.minimum_counter;

			// avoid breaking digger music
			spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
			spkr.pit_mode = 6; // dummy mode with constant high output
			AddPITOutput(newindex);
			return;
		}
		spkr.pit_new_max = duration_of_count_ms;
		spkr.pit_new_half = spkr.pit_new_max / 2;
		if (!spkr.pit_mode3_counting) {
			spkr.pit_index = 0;
			spkr.pit_max = spkr.pit_new_max;
			spkr.pit_half = spkr.pit_new_half;
			if (spkr.pit_clock_gate_enabled) {
				spkr.pit_mode3_counting = 1;
				// probably not necessary
				spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
				AddPITOutput(newindex);
			}
		}
		break;
	case 4: /* Software triggered strobe */
		spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
		AddPITOutput(newindex);
		spkr.pit_index = 0;
		spkr.pit_max = duration_of_count_ms;
		break;
	default:
#ifdef SPKR_DEBUGGING
		LOG_MSG("Unhandled speaker mode %d at %f", mode, PIC_FullIndex());
#endif
		return;
	}
	spkr.pit_mode = mode;
}

void PCSPEAKER_SetType(const bool pit_clock_gate_enabled, const bool pit_output_enabled)
{
#ifdef SPKR_DEBUGGING
	LOG_INFO("PCSPEAKER: %f output: %s, clock gate %s",
	        PIC_FullIndex(),
	        pit_output_enabled ? "pit" : "forced low",
	        pit_clock_gate_enabled ? "on" : "off");
#endif
	// if (!spkr.last_ticks) {
	//	if(spkr.chan) spkr.chan->Enable(true);
	//	spkr.last_index=0;
	// }
	// spkr.last_ticks=PIC_Ticks;
	const float newindex = static_cast<float>(PIC_TickIndex());
	ForwardPIT(newindex);
	// pit clock gate enable rising edge is a trigger
	const bool pit_trigger = pit_clock_gate_enabled &&
	                         !spkr.pit_clock_gate_enabled;
	spkr.pit_clock_gate_enabled = pit_clock_gate_enabled;
	spkr.pit_output_enabled = pit_output_enabled;
	if (pit_trigger) {
		switch (spkr.pit_mode) {
		case 1:
			if (spkr.pit_mode1_waiting_for_counter) {
				// assert output level is high
				break;
			}
			spkr.pit_output_level = SPKR_NEGATIVE_LEVEL;
			spkr.pit_index = 0;
			spkr.pit_max = spkr.pit_mode1_pending_max;
			spkr.pit_mode1_waiting_for_trigger = 0;
			break;
		case 3:
			spkr.pit_mode3_counting = 1;
			// spkr.pit_new_max = spkr.pit_new_max; // typo or bug?
			spkr.pit_new_half = spkr.pit_new_max / 2;
			spkr.pit_index = 0;
			spkr.pit_max = spkr.pit_new_max;
			spkr.pit_half = spkr.pit_new_half;
			spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
			break;
		default:
			// TODO: implement other modes
			break;
		}
	} else if (!pit_clock_gate_enabled) {
		switch (spkr.pit_mode) {
		case 1:
			// gate level does not affect mode1
			break;
		case 3:
			// low gate forces pit output high
			spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
			spkr.pit_mode3_counting = 0;
			break;
		default:
			// TODO: implement other modes
			break;
		}
	}
	if (pit_output_enabled) {
		AddDelayEntry(newindex, spkr.pit_output_level);
	} else {
		AddDelayEntry(newindex, SPKR_NEGATIVE_LEVEL);
	}
}

// TODO: check if this is accurate
static inline float sinc(const float t)
{
#define SINC_ACCURACY 20
	float result = 1;
	for (auto k = 1; k < SINC_ACCURACY; ++k) {
		result *= cosf(t / powf(2.0f, static_cast<float>(k)));
	}
	return result;
}

static inline float impulse(const float t)
{
	// raised-cosine-windowed sinc function
	const float fs = spkr.rate_as_float;
	const float fc = fs / (2 + SPKR_CUTOFF_MARGIN);
	const float q = SPKR_FILTER_QUALITY;
	if ((0 < t) && (t * fs < q)) {
		float window = 1.0f + cosf(2 * fs * pi_f * (q / (2 * fs) - t) / q);
		return window * (sinc(2 * fc * pi_f * (t - q / (2 * fs)))) / 2.0f;
	} else
		return 0.0f;
}

static inline void add_impulse(const float index, const float amplitude)
{
#ifndef REFERENCE
	const auto samples_in_impulse = index * spkr.rate_per_ms;
	auto offset = static_cast<uint32_t>(samples_in_impulse);
	auto phase = static_cast<uint32_t>(samples_in_impulse * SPKR_OVERSAMPLING) %
	             SPKR_OVERSAMPLING;
	if (phase != 0) {
		offset++;
		phase = SPKR_OVERSAMPLING - phase;
	}
	for (uint16_t i = 0; i < SPKR_FILTER_QUALITY; ++i) {
		assertm(offset + i < spkr.output_buffer.size(),
		        "index into spkr.output_buffer too high");
		assertm(phase + SPKR_OVERSAMPLING * i < SPKR_FILTER_WIDTH,
		        "index into spkr.sampled_impulse too high");
		spkr.output_buffer[offset + i] +=
		        amplitude *
		        spkr.sampled_impulse[phase + i * SPKR_OVERSAMPLING];
	}
}
#else
	const auto portion_of_ms = index / 1000.0f;
	for (size_t i = 0; i < spkr.output_buffer.size(); ++i) {
		const auto impulse_time = i / spkr.rate_as_float - portion_of_ms;
		spkr.output_buffer[i] += amplitude * impulse(impulse_time);
	}
}
#endif

static void PCSPEAKER_CallBack(uint16_t len)
{
	static std::vector<int16_t> mix_buffer;
	if (mix_buffer.size() < len)
		mix_buffer.resize(len);
	auto stream = mix_buffer.begin();

	ForwardPIT(1.0f);
	spkr.last_index = 0;
	for (auto i = 0; i < spkr.used; ++i) {
		const float index = clamp(spkr.entries[i].index, 0.0f, 1.0f);
		add_impulse(index, spkr.entries[i].output_level);
	}
	spkr.used = 0;
	if (len > spkr.output_buffer.size()) {
		// massive HACK, insert zeros if callback wants too many
		// samples
		LOG_MSG("mixer callback wants too many samples from pc speaker emulator: %u",
		        len);
		for (size_t i = len - spkr.output_buffer.size(); i; i--) {
			*stream++ = 0; // output
		}
		len = check_cast<uint16_t>(spkr.output_buffer.size());
	}
	// "consume" output buffer
	static float current_output_level = 0.0f;
	for (unsigned i = 0; i < len; ++i) {
		current_output_level += spkr.output_buffer[i];
		assert(current_output_level >= MIN_AUDIO && current_output_level <= MAX_AUDIO);
		*stream++ = static_cast<int16_t>(current_output_level); // output sample
		current_output_level *= SPKR_HIGHPASS;
	}
	// shift out consumed samples
	// TODO: use ring buffer or something to avoid shifting
	for (unsigned i = len; i < spkr.output_buffer.size(); ++i) {
		assertm(i - len < spkr.output_buffer.size(),
		        "index into spkr.output_buffer too high");
		assertm(i < spkr.output_buffer.size(),
		        "index into spkr.output_buffer too high");
		spkr.output_buffer[i - len] = spkr.output_buffer[i];
	}
	// zero the rest of the samples
	for (size_t i = spkr.output_buffer.size() - len;
	     i < spkr.output_buffer.size();
	     ++i) {
		spkr.output_buffer[i] = 0.0f;
	}
	if (spkr.chan)
		spkr.chan->AddSamples_m16(len, mix_buffer.data());

		// Turn off speaker after 10 seconds of idle or one
		// second idle when in off mode bool turnoff = false;
		// Bitu test_ticks = PIC_Ticks; if ((spkr.last_ticks +
		// 10000) < test_ticks) turnoff = true;
		// if((!spkr.pit_output_enabled) &&
		// ((spkr.last_ticks + 1000) < test_ticks)) turnoff =
		// true;

		// if(turnoff){
		//	if(spkr.volwant == 0) {
		//		spkr.last_ticks = 0;
		//		if(spkr.chan) spkr.chan->Enable(false);
		//	} else {
		//		if(spkr.volwant > 0) spkr.volwant--; else
		// spkr.volwant++;
		//
		//	}
		// }
#ifdef SPKR_DEBUGGING
	if (spkr.used) {
		LOG_MSG("PCSPEAKER_CallBack: DelayEntries not emptied (%u) at %f",
		        spkr.used,
		        PIC_FullIndex());
	}
#endif
}

static void init_interpolation()
{
	spkr.sampled_impulse.resize(SPKR_FILTER_WIDTH);
	for (uint16_t i = 0; i < SPKR_FILTER_WIDTH; ++i) {
		spkr.sampled_impulse[i] = impulse(
		        i / (spkr.rate_as_float * SPKR_OVERSAMPLING));
	}
	// FILE* asdf = fopen("dummkopf.raw", "wb");
	// fwrite(spkr.sampled_impulse, sizeof(float), SPKR_FILTER_WIDTH,
	// asdf); fclose(asdf);
	//  +1 to compensate for rounding down of the division
	const auto output_buffer_length = check_cast<uint16_t>(
	        SPKR_FILTER_QUALITY + spkr.rate / 1000 + 1);
	spkr.output_buffer.resize(output_buffer_length, 0.0f);
	// DEBUG
	LOG_MSG("PC speaker output buffer length: %u", output_buffer_length);
}

class PCSPEAKER final : public Module_base {
public:
	PCSPEAKER(Section *configuration) : Module_base(configuration)
	{
		spkr.chan = 0;
		Section_prop *section = static_cast<Section_prop *>(configuration);
		if (!section->Get_bool("pcspeaker"))
			return;
		spkr.pit_output_enabled = 0;
		spkr.pit_clock_gate_enabled = 0;
		spkr.pit_mode1_waiting_for_trigger = 1;
		// spkr.last_ticks=0;
		spkr.last_index = 0;

		spkr.rate = std::max(section->Get_int("pcrate"), 8000);;
		spkr.rate_as_float = static_cast<float>(spkr.rate);
		spkr.rate_per_ms = spkr.rate_as_float / 1000.0f;
		init_interpolation();

		// PIT initially in mode 3 at ~903 Hz
		spkr.pit_mode = 3;
		spkr.pit_mode3_counting = 0;
		spkr.pit_output_level = SPKR_POSITIVE_LEVEL;
		spkr.pit_max = ms_per_pit_tick * 1320;
		spkr.pit_half = spkr.pit_max / 2;
		spkr.pit_new_max = spkr.pit_max;
		spkr.pit_new_half = spkr.pit_half;
		spkr.pit_index = 0;

		// spkr.minimum_counter = (PIT_TICK_RATE +
		// spkr.rate/2-1)/(spkr.rate/2);
		spkr.minimum_counter = 2 * PIT_TICK_RATE / spkr.rate;
		spkr.used = 0;
		/* Register the sound channel */

		spkr.chan = MIXER_AddChannel(&PCSPEAKER_CallBack,
		                             spkr.rate,
		                             "SPKR",
		                             {ChannelFeature::ReverbSend,
		                              ChannelFeature::ChorusSend});
		spkr.chan->SetPeakAmplitude(
		        static_cast<uint32_t>(AMPLITUDE_POSITIVE));
		spkr.chan->Enable(true);
	}
	~PCSPEAKER()
	{
		Section_prop *section = static_cast<Section_prop *>(m_configuration);
		if (!section->Get_bool("pcspeaker"))
			return;
	}
};
static PCSPEAKER *test;

void PCSPEAKER_ShutDown([[maybe_unused]] Section *sec)
{
	delete test;
}

void PCSPEAKER_Init(Section *sec)
{
	test = new PCSPEAKER(sec);
	sec->AddDestroyFunction(&PCSPEAKER_ShutDown, true);
}
