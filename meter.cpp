#include <cassert>
#include <cmath>                                                // std::isnan()

#include "meter.h"

using namespace Meter;

// Round n to the given number of decimal places and return the result.
static double round(double n, int decimalPlaces)
{
    return floor(pow(10, decimalPlaces) * n) / pow(10, decimalPlaces);
}

// Initialise the Phase with the given values.
void Phase::set(double vrms_, double irms_, double powerActive_, double powerReactive_, double powerFactor_)
{
    vrms = vrms_;
    irms = irms_;
    powerActive = powerActive_;
    powerReactive = powerReactive_;
    powerFactor = powerFactor_;
}

// Return true if all required members are present in the powerQualityData, false otherwise.
bool Sample::isValid(const xsd::mtrsvc::PowerQualityData& powerQualityData)
{
    if (powerQualityData.voltageA.empty() || powerQualityData.currentA.empty() || powerQualityData.activePowerA.empty()
        || powerQualityData.reactivePowerA.empty() || powerQualityData.powerFactorA.empty() || powerQualityData.voltageB.empty()
        || powerQualityData.currentB.empty() || powerQualityData.activePowerB.empty() || powerQualityData.reactivePowerB.empty()
        || powerQualityData.powerFactorB.empty() || powerQualityData.voltageC.empty() || powerQualityData.currentC.empty()
        || powerQualityData.activePowerC.empty() || powerQualityData.reactivePowerC.empty() || powerQualityData.powerFactorC.empty()
        || powerQualityData.frequency.empty())
        return false;

    return true;
}

// Initialise the Sample from a PowerQualityData object.
void Sample::set(const xsd::mtrsvc::PowerQualityData& powerQualityData)
{
    p1.set(powerQualityData.voltageA.getValue(), powerQualityData.currentA.getValue(), powerQualityData.activePowerA.getValue(),
           powerQualityData.reactivePowerA.getValue(), powerQualityData.powerFactorA.getValue());
    p2.set(powerQualityData.voltageB.getValue(), powerQualityData.currentB.getValue(), powerQualityData.activePowerB.getValue(),
           powerQualityData.reactivePowerB.getValue(), powerQualityData.powerFactorB.getValue());
    p3.set(powerQualityData.voltageC.getValue(), powerQualityData.currentC.getValue(), powerQualityData.activePowerC.getValue(),
           powerQualityData.reactivePowerC.getValue(), powerQualityData.powerFactorC.getValue());
    frequency = powerQualityData.frequency.getValue();
}

// Create a JSON "h" array encoding the histogram state.
void Histogram::json(ordered_json& j) const
{
    j.clear();
    j["h"] = { bin[0], bin[1], bin[2], bin[3], bin[4], bin[5], bin[6], bin[7], bin[8], bin[9], bin[10], bin[11] };
}

// Create a JSON object encoding the average, minimum, maximum, and histogram.
void Summary::json(ordered_json& j) const
{
    j.clear();
    j["avg"] = avg;
    j["min"] = min;
    j["max"] = max;
    j["h"] = { histogram.bin[0], histogram.bin[1], histogram.bin[2], histogram.bin[3], histogram.bin[4], histogram.bin[5],
               histogram.bin[6], histogram.bin[7], histogram.bin[8], histogram.bin[9], histogram.bin[10], histogram.bin[11] };
}

// Create a JSON object encoding the voltage, current, and active and reactive power.
void PhaseSummary::json(ordered_json& j) const
{
    j.clear();
    ordered_json tmp;
    vrms.json(tmp);
    j["v"] = tmp;
    irms.json(tmp);
    j["i"] = tmp;
    powerActive.json(tmp);
    j["p"] = tmp;
    powerReactive.json(tmp);
    j["q"] = tmp;
    powerFactor.json(tmp);
    j["pf"] = tmp;
}

// Create a JSON object encoding the summary over the past n sample intervals.
void SampleSummary::json(ordered_json& j) const
{
    j.clear();
    j["p"] = json::array();
    ordered_json tmp;
    p1.json(tmp);
    j["p"][0] = tmp;
    p2.json(tmp);
    j["p"][1] = tmp;
    p3.json(tmp);
    j["p"][2] = tmp;
    frequency.json(tmp);
    j["f"] = tmp;
    j["n"] = count;
    j["ts"] = duration_cast<seconds>(tsStart.time_since_epoch()).count();
    j["te"] = duration_cast<seconds>(tsEnd.time_since_epoch()).count();
}

// Accumulate the given sample.
bool Report::accumulate(const Sample& sample)
{
    return acc.accumulate(sample);
}

// Summarise all accumulated samples into the given sample summary.
bool Report::summarise(SampleSummary& sampleSummary)
{
    return acc.summarise(sampleSummary);
}

uint32_t Report::count()
{
    return acc.count;
}

void Report::reset()
{
    acc.reset();
}

// Accumulate the given value into the total, min and/or max if appropriate, and histogram.
bool Report::Accumulator::accumulate(const double val)
{
    if (binBoundaryPtr == nullptr)
    {
        assert(false);
        return false;
    }

    uint32_t bin;
    for (bin = 0; bin < HISTOGRAM_BINS; bin++)
    {
        if (val < binBoundaryPtr[bin])
            break;
    }

    if (bin < HISTOGRAM_BINS)
    {
        histogram.bin[bin]++;
        total += val;
        if (val < min || std::isnan(min))
            min = val;
        if (val > max || std::isnan(max))
            max = val;

        return true;
    }

    return false;
}

// Summarise the accumulated values over the given number of samples into the provided summary.
bool Report::Accumulator::summarise(Summary& summary, uint32_t count) const
{
    if (count == 0 || memcpy(&summary.histogram.bin, &histogram.bin, HISTOGRAM_BINS * sizeof (uint32_t)) == NULL)
    {
        assert(false);
        return false;
    }

    summary.avg = round(total / count, decimalPlaces);
    summary.min = round(min, decimalPlaces);
    summary.max = round(max, decimalPlaces);

    return true;
}

// Accumulate the given phase point-in-time data.
// Returns true if all components were successfully added, false if at least one failed.
bool Report::PhaseAccumulator::accumulate(const Phase& phase)
{
    bool successV = vrms.accumulate(phase.vrms);
    bool successI = irms.accumulate(phase.irms);
    bool successP = powerActive.accumulate(phase.powerActive);
    bool successQ = powerReactive.accumulate(phase.powerReactive);
    bool successF = powerFactor.accumulate(phase.powerFactor);

    if (successV && successI && successP && successQ && successF)
        return true;

    assert(false);
    return false;
}

// Summarise the accumulated phase values over the given number of samples into the provided phase summary.
bool Report::PhaseAccumulator::summarise(PhaseSummary& phaseSummary, uint32_t count) const
{
    bool successV = vrms.summarise(phaseSummary.vrms, count);
    bool successI = irms.summarise(phaseSummary.irms, count);
    bool successP = powerActive.summarise(phaseSummary.powerActive, count);
    bool successQ = powerReactive.summarise(phaseSummary.powerReactive, count);
    bool successF = powerFactor.summarise(phaseSummary.powerFactor, count);

    if (successV && successI && successP && successQ && successF)
        return true;

    assert(false);
    return false;
}

// Accumulate the given all-phase point-in-time data, and increment the count (unless all the accumulations failed).
// Returns true if all components were successfully added, false if at least one failed.
bool Report::SampleAccumulator::accumulate(const Sample& sample)
{
    bool successP1 = p1.accumulate(sample.p1);
    bool successP2 = p2.accumulate(sample.p2);
    bool successP3 = p3.accumulate(sample.p3);
    bool successF = frequency.accumulate(sample.frequency);

    if (successP1 && successP2 && successP3 && successF)
    {
        count++;
        tsEnd = system_clock::now();
        return true;
    }

    assert(false);
    return false;
}

// Summarise all accumulated samples into the provided sample summary.
// NOTE If any of the summaries fail, this will leave incorrect data in sampleSummary, but return false.
bool Report::SampleAccumulator::summarise(SampleSummary& sampleSummary) const
{
    bool successP1 = p1.summarise(sampleSummary.p1, count);
    bool successP2 = p2.summarise(sampleSummary.p2, count);
    bool successP3 = p3.summarise(sampleSummary.p3, count);
    bool successF = frequency.summarise(sampleSummary.frequency, count);
    sampleSummary.count = count;
    sampleSummary.tsStart = tsStart;
    sampleSummary.tsEnd = tsEnd;

    if (successP1 && successP2 && successP3 && successF)
        return true;

    assert(false);
    return false;
}
