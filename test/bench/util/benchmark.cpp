#include "benchmark.hpp"

#include "results.hpp"      // Results
#include "timer.hpp"        // Timer

using namespace realm;
using namespace realm::test_util;

std::string Benchmark::lead_text()
{
    std::stringstream stream;
    stream << this->name() << " (MemOnly, EncryptionOff)";
    return stream.str();
}

std::string Benchmark::ident()
{
    std::stringstream stream;
    stream << this->name() << "_MemOnly_EncryptionOff";
    return stream.str();
}

void Benchmark::run_once(SharedGroup& sg, Timer& timer)
{
    timer.pause();
    this->before_each(sg);
    timer.unpause();

    this->operator()(sg);

    timer.pause();
    this->after_each(sg);
    timer.unpause();
}

void Benchmark::run(Results& results)
{
    std::string lead_text = this->lead_text();
    std::string ident = this->ident();

    std::string realm_path = "results.realm";
    std::unique_ptr<SharedGroup> sg;
    sg.reset(new SharedGroup(realm_path, false,
                             SharedGroup::durability_MemOnly, nullptr));

    this->before_all(*sg);

    Timer t;
    this->run_once(*sg, t);
    results.submit(ident.c_str(), t.get_elapsed_time());

    this->after_all(*sg);

    results.finish(ident, lead_text);
}