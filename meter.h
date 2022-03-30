// Meter sample handling, statically allocated memory version.
//
// Usage:
//
//    using namespace Meter;
//    Report report;
//    Sample sample;
//    SampleSummary sampleSummary;
//    while (...)
//    {
//        sample = ...;
//        report.accumulate(sample);
//        sample.reset();
//    }
//    report.summarise(sampleSummary);
//    report.reset();

#pragma once

#include <string.h>                                             // memcpy(), memset()
#include <math.h>                                               // NAN, HUGE_VAL

#include <cstdint>
#include <chrono>

#include <xsd/mtrsvc/MeterReadSchedulePolicy.hpp>               // PowerQualityData

#include "json.hpp"

using namespace std::chrono;
using namespace nlohmann;

#define EXPECTED_VOLTAGE    230                                 // Options: 110 (for 100/110/120 V), 230 (for 220/230/240 V)
#define EXPECTED_FREQUENCY   50                                 // Options: 50, 60

namespace Meter
{
    static constexpr uint32_t HISTOGRAM_BINS = 12;

    // Boundaries of the histogram bins for Vrms, Irms, active power, reactive power, power factor, and frequency.
    // Each value is the upper bound for its corresponding bin, e.g. voltages [215.0..220.0) will count toward bin[3].
    // The last value is HUGE_VAL (+infinity) in order to match any remaining value except +infinity or NaN.
#if EXPECTED_VOLTAGE == 110
    static constexpr double binBoundaryV[HISTOGRAM_BINS]  = {     85.0,    90.0,    95.0,   100.0,   105.0,   110.0,
                                                                 115.0,   120.0,   125.0,   130.0,   135.0,      HUGE_VAL };
#elif EXPECTED_VOLTAGE == 230
    static constexpr double binBoundaryV[HISTOGRAM_BINS]  = {    205.0,   210.0,   215.0,   220.0,   225.0,   230.0,
                                                                 235.0,   240.0,   245.0,   250.0,   255.0,      HUGE_VAL };
#endif
    static constexpr double binBoundaryI[HISTOGRAM_BINS]  = {      0.05,    0.1,     0.2,     0.5,     1.0,     2.0,
                                                                   5.0,    10.0,    20.0,    50.0,   100.0,      HUGE_VAL };
    static constexpr double binBoundaryP[HISTOGRAM_BINS]  = { -10000.0, -3000.0, -1000.0,  -300.0,  -100.0,     0.0,
                                                                 100.0,   300.0,  1000.0,  3000.0, 10000.0,      HUGE_VAL };
    static constexpr double binBoundaryQ[HISTOGRAM_BINS]  = { -10000.0, -3000.0, -1000.0,  -300.0,  -100.0,     0.0,
                                                                 100.0,   300.0,  1000.0,  3000.0, 10000.0,      HUGE_VAL };
    static constexpr double binBoundaryPF[HISTOGRAM_BINS] = {     -1.0,    -0.8,    -0.6,    -0.4,    -0.2,     0.0,
                                                                   0.2,     0.4,     0.6,     0.8,     1.0,      HUGE_VAL };
#if EXPECTED_FREQUENCY == 50
    static constexpr double binBoundaryF[HISTOGRAM_BINS]  = {     49.75,   49.80,   49.85,   49.90,   49.95,   50.00,
                                                                  50.05,   50.10,   50.15,   50.20,   50.25,     HUGE_VAL };
#elif EXPECTED_FREQUENCY == 60
    static constexpr double binBoundaryF[HISTOGRAM_BINS]  = {     59.75,   59.80,   59.85,   59.90,   59.95,   60.00,
                                                                  60.05,   60.10,   60.15,   60.20,   60.25,     HUGE_VAL };
#endif

    // Point-in-time voltage/current/power of a single phase.
    struct Phase
    {
        Phase() { reset(); }
        Phase(double vrms_, double irms_, double powerActive_, double powerReactive_, double powerFactor_) {
            set(vrms_, irms_, powerActive_, powerReactive_, powerFactor_); }
        void set(double vrms_, double irms_, double powerActive_, double powerReactive_, double powerFactor_);
        void reset() { vrms = irms = powerActive = powerReactive = powerFactor = 0.0; }

        double vrms;
        double irms;
        double powerActive;
        double powerReactive;
        double powerFactor;
    };

    // Point-in-time voltage/current/power of up to three phases, plus frequency derived from Phase 1.
    struct Sample
    {
        Sample() { frequency = 0.0; }
        Sample(const xsd::mtrsvc::PowerQualityData& powerQualityData) { set(powerQualityData); }
        static bool isValid(const xsd::mtrsvc::PowerQualityData& powerQualityData);
        void set(const xsd::mtrsvc::PowerQualityData& powerQualityData);
        void reset() { p1.reset(); p2.reset(); p3.reset(); frequency = 0.0; }

        Phase p1;
        Phase p2;
        Phase p3;
        double frequency;
    };

    // Histogram with a fixed number of bins, representing the number of times values have fallen into certain ranges.
    struct Histogram
    {
        Histogram() { reset(); }
        void json(ordered_json& j) const;
        void reset() { memset(&bin, 0, HISTOGRAM_BINS * sizeof (uint32_t)); }

        uint32_t bin[HISTOGRAM_BINS];
    };

    // Summary of zero or more doubles, with their average, minimum and maximum values, and a histogram.
    struct Summary
    {
        Summary() { avg = 0.0; min = max = NAN; }
        void json(ordered_json& j) const;
        void reset() { histogram.reset(); avg = 0.0; min = max = NAN; }

        Histogram histogram;
        double avg;
        double min;
        double max;
    };

    // Summary voltage/current/power of a single phase.
    struct PhaseSummary
    {
        void json(ordered_json& j) const;
        void reset() { vrms.reset(); irms.reset(); powerActive.reset(); powerReactive.reset(); powerFactor.reset(); }

        Summary vrms;
        Summary irms;
        Summary powerActive;
        Summary powerReactive;
        Summary powerFactor;
    };

    // Summary voltage/current/power of up to three phases plus frequency, as well as the count of samples covered.
    struct SampleSummary
    {
        SampleSummary() { count = 0; }
        void json(ordered_json& j) const;
        void reset() { p1.reset(); p2.reset(); p3.reset(); frequency.reset(); count = 0; intervalMin = milliseconds(INT32_MAX);
                       intervalMax = milliseconds(0); tsStart = tsEnd = system_clock::now(); }

        PhaseSummary p1;
        PhaseSummary p2;
        PhaseSummary p3;
        Summary frequency;
        uint32_t count;
        milliseconds intervalMin;
        milliseconds intervalMax;
        time_point<system_clock> tsStart;
        time_point<system_clock> tsEnd;
    };

    class Report
    {
    public:
        Report() { reset(); }
        ~Report() = default;
        bool accumulate(const Sample& sample);
        bool summarise(SampleSummary& sampleSummary);
        uint32_t count();
        void reset();

    private:
        // Accumulated doubles, with their total, minimum and maximum values, and a histogram.
        // Derived classes override binBoundaryPtr to establish different histogram bins; it is not intended to use this base class
        // directly.
        struct Accumulator
        {
            Accumulator() { total = 0.0; min = max = NAN; }
            bool accumulate(const double val);
            bool summarise(Summary& summary, uint32_t count) const;
            void reset() { histogram.reset(); total = 0.0; min = max = NAN; }

            Histogram histogram;
            double total;
            double min;
            double max;

        protected:
            const double* binBoundaryPtr = nullptr;             // Histogram bin boundaries, overridden by derived classes
            int decimalPlaces = 0;                              // Number of decimal places to round to, set by derived classes
        };

        // Accumulated RMS voltage values.
        struct AccumulatorVrms : Accumulator
        {
            AccumulatorVrms() { binBoundaryPtr = binBoundaryV; decimalPlaces = 1; }
        };

        // Accumulated RMS current values.
        struct AccumulatorIrms : Accumulator
        {
            AccumulatorIrms() { binBoundaryPtr = binBoundaryI; decimalPlaces = 2; }
        };

        // Accumulated active power values.
        struct AccumulatorPowerActive : Accumulator
        {
            AccumulatorPowerActive() { binBoundaryPtr = binBoundaryP; decimalPlaces = 1; }
        };

        // Accumulated reactive power values.
        struct AccumulatorPowerReactive : Accumulator
        {
            AccumulatorPowerReactive() { binBoundaryPtr = binBoundaryQ; decimalPlaces = 1; }
        };

        // Accumulated power factor values.
        struct AccumulatorPowerFactor : Accumulator
        {
            AccumulatorPowerFactor() { binBoundaryPtr = binBoundaryPF; decimalPlaces = 2; }
        };

        // Accumulated frequency values.
        struct AccumulatorFrequency : Accumulator
        {
            AccumulatorFrequency() { binBoundaryPtr = binBoundaryF; decimalPlaces = 1; }
        };

        // Accumulated voltage/current/power of a single phase.
        struct PhaseAccumulator
        {
            bool accumulate(const Phase& phase);
            bool summarise(PhaseSummary& summary, uint32_t count) const;
            void reset() { vrms.reset(); irms.reset(); powerActive.reset(); powerReactive.reset(); powerFactor.reset(); }

            AccumulatorVrms vrms;
            AccumulatorIrms irms;
            AccumulatorPowerActive powerActive;
            AccumulatorPowerReactive powerReactive;
            AccumulatorPowerFactor powerFactor;
        };

        // Accumulated voltage/current/power of up to three phases, frequency, and the count and timestamps.
        struct SampleAccumulator
        {
            SampleAccumulator() { count = 0; }
            bool accumulate(const Sample& sample);
            bool summarise(SampleSummary& sampleSummary) const;
            void reset() { p1.reset(); p2.reset(); p3.reset(); frequency.reset(); count = 0; intervalMin = milliseconds(INT32_MAX);
                           intervalMax = milliseconds(0); tsLast = tsStart = tsEnd = system_clock::now(); }

            PhaseAccumulator p1;
            PhaseAccumulator p2;
            PhaseAccumulator p3;
            AccumulatorFrequency frequency;
            uint32_t count;
            milliseconds intervalMin;
            milliseconds intervalMax;
            time_point<system_clock> tsLast;
            time_point<system_clock> tsStart;
            time_point<system_clock> tsEnd;
        };

        SampleAccumulator acc;                                  // Statically allocated meter sample accumulator
    };
}
