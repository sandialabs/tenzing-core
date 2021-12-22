#pragma once

#include <vector>
#include <string>

#include "schedule.hpp"
#include "sequence.hpp"

struct Benchmark {
    struct Result {
        double pct01;
        double pct10;
        double pct50;
        double pct90;
        double pct99;
        double stddev;
    };

    struct Opts {

    };
};

/* actually run the code to do the benchmark
*/
struct EmpiricalBenchmarker : public Benchmark {
    Result benchmark(std::vector<std::shared_ptr<BoundOp>> &order, Platform &plat, const BenchOpts &opts = BenchOpts());
    std::vector<Result> benchmark(std::vector<Schedule> &schedules, Platform &plat, const BenchOpts &opts = BenchOpts()); 
};


/* find the results in a loaded database and return them
*/
struct CsvBenchmarker : public Benchmark {

    struct DataRow {
        Result res;
        Sequence<BoundOp> seq;
    };

    // what CSV file to read in and what graph to pull the operations from
    CsvBenchmarker(const std::string &path, const Graph<OpBase> &g);
    Result benchmark(std::vector<std::shared_ptr<BoundOp>> &order, Platform &plat, const BenchOpts &opts = BenchOpts());

    // csv data with operations pulled from the graph (to replace data_)
    std::vector<DataRow> data_;
};