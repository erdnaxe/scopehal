/***********************************************************************************************************************
*                                                                                                                      *
* libscopeprotocols                                                                                                    *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg and contributors                                                         *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

#include "../scopehal/scopehal.h"
#include "../scopehal/LeCroyOscilloscope.h"
#include "TRCImportFilter.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

TRCImportFilter::TRCImportFilter(const string& color)
	: ImportFilter(color)
{
	m_fpname = "TRC File";
	m_parameters[m_fpname] = FilterParameter(FilterParameter::TYPE_FILENAME, Unit(Unit::UNIT_COUNTS));
	m_parameters[m_fpname].m_fileFilterMask = "*.trc";
	m_parameters[m_fpname].m_fileFilterName = "Teledyne LeCroy waveform files (*.trc)";
	m_parameters[m_fpname].signal_changed().connect(sigc::mem_fun(*this, &TRCImportFilter::OnFileNameChanged));
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Accessors

string TRCImportFilter::GetProtocolName()
{
	return "TRC Import";
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Actual decoder logic

void TRCImportFilter::OnFileNameChanged()
{
	auto fname = m_parameters[m_fpname].ToString();
	if(fname.empty())
		return;

	LogTrace("Loading TRC waveform %s\n", fname.c_str());
	LogIndenter li;

	FILE* fp = fopen(fname.c_str(), "r");
	if(!fp)
	{
		LogError("Couldn't open TRC file \"%s\"\n", fname.c_str());
		return;
	}

	//Read the SCPI file length header
	//Expect #9 followed by 9 digit ASCII length
	char header[13] = {0};
	if(11 != fread(header, 1, 11, fp))
	{
		LogError("Failed to read file length header\n");
		fclose(fp);
		return;
	}
	if((header[0] != '#') || (header[1] != '9') )
	{
		//Really long files are #A followed by 10 digit length
		if( (header[0] == '#') && (header[1] == 'A') )
		{
			if(1 != fread(&header[11], 1, 1, fp))
			{
				LogError("Failed to read file length header\n");
				fclose(fp);
				return;
			}
		}

		else
		{
			LogError("Invalid file length header\n");
			fclose(fp);
			return;
		}
	}
	size_t len = stoull(header+2);
	LogTrace("File length from header: %zu bytes\n", len);
	const size_t wavedescSize = 346;
	if(len < wavedescSize)
	{
		LogError("Invalid file length in header (too small for WAVEDESC)\n");
		fclose(fp);
		return;
	}

	//Read the WAVEDESC
	uint8_t wavedesc[wavedescSize];
	if(wavedescSize != fread(wavedesc, 1, wavedescSize, fp))
	{
		LogError("Failed to read WAVEDESC\n");
		fclose(fp);
		return;
	}

	//Validate the WAVEDESC
	if(0 != memcmp(wavedesc, "WAVEDESC", 8))
	{
		LogError("Malformed WAVEDESC (magic number is wrong)\n");
		fclose(fp);
		return;
	}

	//Figure out sample resolution
	bool hdMode = (wavedesc[32] != 0);
	if(hdMode)
		LogTrace("Sample format:           int16_t\n");
	else
		LogTrace("Sample format:           int8_t\n");

	//Assume little endian for now

	//Get instrument format
	char instName[17] = {0};
	memcpy(instName, wavedesc + 76, 16);
	LogTrace("Instrument name:         %s\n", instName);

	//cppcheck-suppress invalidPointerCast
	float v_gain = *reinterpret_cast<float*>(wavedesc + 156);

	//cppcheck-suppress invalidPointerCast
	float v_off = *reinterpret_cast<float*>(wavedesc + 160);

	//cppcheck-suppress invalidPointerCast
	float interval = *reinterpret_cast<float*>(wavedesc + 176) * FS_PER_SECOND;

	//cppcheck-suppress invalidPointerCast
	double h_off = *reinterpret_cast<double*>(wavedesc + 180) * FS_PER_SECOND;	//fs from start of waveform to trigger

	double h_off_frac = fmodf(h_off, interval);						//fractional sample position, in fs
	if(h_off_frac < 0)
		h_off_frac = interval + h_off_frac;		//double h_unit = *reinterpret_cast<double*>(pdesc + 244);

	//Get the waveform timestamp
	double basetime;
	auto ttime = LeCroyOscilloscope::ExtractTimestamp(wavedesc, basetime);

	//TODO: support sequence mode .trc files here??

	//Set up output stream
	//Channel number is byte at offset 344 (zero based)
	ClearStreams();
	string chName = string("C") + to_string(wavedesc[344] + 1);
	AddStream(Unit(Unit::UNIT_VOLTS), chName, Stream::STREAM_TYPE_ANALOG);
	m_outputsChangedSignal.emit();

	//Figure out length of actual waveform data
	size_t datalen = len - wavedescSize;
	size_t num_samples;
	if(hdMode)
		num_samples = datalen/2;
	else
		num_samples = datalen;
	size_t num_per_segment = num_samples /* / num_sequences*/;

	//Create output waveform
	auto wfm = new AnalogWaveform;
	wfm->m_timescale = round(interval);
	wfm->m_startTimestamp = ttime;
	wfm->m_startFemtoseconds = basetime * FS_PER_SECOND;
	wfm->m_triggerPhase = h_off_frac;
	wfm->m_densePacked = true;
	SetData(wfm, 0);
	wfm->Resize(num_per_segment);

	//16 bit sample path
	if(hdMode)
	{
		AcceleratorBuffer<int16_t> buf;
		buf.resize(num_per_segment);
		buf.PrepareForCpuAccess();

		if(num_per_segment != fread(&buf[0], sizeof(int16_t), num_per_segment, fp))
		{
			LogError("Failed to read sample data\n");
			fclose(fp);
			return;
		}

		Oscilloscope::Convert16BitSamples(
			(int64_t*)&wfm->m_offsets[0],
			(int64_t*)&wfm->m_durations[0],
			(float*)&wfm->m_samples[0],
			&buf[0],
			v_gain,
			v_off,
			num_per_segment,
			0);

		wfm->m_offsets.MarkModifiedFromCpu();
		wfm->m_durations.MarkModifiedFromCpu();
		wfm->m_samples.MarkModifiedFromCpu();
	}

	//8 bit sample path
	else
	{
		AcceleratorBuffer<int8_t> buf;
		buf.resize(num_per_segment);
		buf.PrepareForCpuAccess();

		if(num_per_segment != fread(&buf[0], sizeof(int8_t), num_per_segment, fp))
		{
			LogError("Failed to read sample data\n");
			fclose(fp);
			return;
		}

		Oscilloscope::Convert8BitSamples(
			(int64_t*)&wfm->m_offsets[0],
			(int64_t*)&wfm->m_durations[0],
			(float*)&wfm->m_samples[0],
			&buf[0],
			v_gain,
			v_off,
			num_per_segment,
			0);

		wfm->m_offsets.MarkModifiedFromCpu();
		wfm->m_durations.MarkModifiedFromCpu();
		wfm->m_samples.MarkModifiedFromCpu();
	}
}
