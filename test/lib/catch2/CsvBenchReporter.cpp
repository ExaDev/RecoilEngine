/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <catch_amalgamated.hpp>

#include <string>

namespace {

// Wrap a string in double-quotes, escaping any embedded double-quotes.
std::string CsvQuote(const std::string& s)
{
	std::string out;
	out.reserve(s.size() + 2);
	out += '"';
	for (char c : s) {
		if (c == '"')
			out += '"'; // RFC 4180: double the quote
		out += c;
	}
	out += '"';
	return out;
}

std::string TrimRight(std::string s)
{
	while (!s.empty() && s.back() == ' ')
		s.pop_back();
	return s;
}

// Parse a benchmark name that embeds the impl and N value, e.g.:
//   "N=100    spring "  → benchOut="N=100", implOut="spring"
//   "N=10000  unsynced" → benchOut="N=10000", implOut="unsynced"
// If the name doesn't match this pattern, benchOut=trimmed name, implOut="".
void ParseBenchName(const std::string& name, std::string& benchOut, std::string& implOut)
{
	// Find the end of the "N=<digits>" prefix
	size_t i = 0;
	if (i + 2 <= name.size() && name[0] == 'N' && name[1] == '=') {
		i = 2;
		while (i < name.size() && (name[i] >= '0' && name[i] <= '9'))
			++i;
		benchOut = name.substr(0, i); // "N=100"

		// Skip spaces to find the impl name
		while (i < name.size() && name[i] == ' ')
			++i;

		// Read the impl name (non-space characters)
		size_t implStart = i;
		while (i < name.size() && name[i] != ' ')
			++i;

		implOut = name.substr(implStart, i - implStart);
	} else {
		benchOut = TrimRight(name);
		implOut.clear();
	}
}

class CsvBenchReporter final : public Catch::StreamingReporterBase {
	std::string currentTestCase;

public:
	using StreamingReporterBase::StreamingReporterBase;

	static std::string getDescription()
	{
		return "CSV benchmark reporter for import into Excel/spreadsheets";
	}

	void testRunStarting(Catch::TestRunInfo const& info) override
	{
		StreamingReporterBase::testRunStarting(info);
		m_stream << "Container,Workload,Impl,Benchmark,Mean (ns),Iterations,Samples\n";
	}

	void testCaseStarting(Catch::TestCaseInfo const& info) override
	{
		StreamingReporterBase::testCaseStarting(info);
		currentTestCase = info.name;
	}

	void benchmarkEnded(Catch::BenchmarkStats<> const& stats) override
	{
		const double meanNs   = stats.mean.point.count();
		const int64_t iters   = static_cast<int64_t>(stats.info.iterations);
		const int64_t samples = static_cast<int64_t>(stats.info.samples);

		// m_sectionStack layout (Catch2 always pushes a root section = test case name):
		//   [0] root (same name as test case)      — skip
		//   [1] workload  e.g. "90% find / 10% mutate"
		//   [2] impl      e.g. "std", "spring"      (scaling benchmark only)
		//
		// Main benchmark uses only one real section level (workload) and embeds
		// the impl in the benchmark name, e.g. "N=100    spring ".

		std::string workload = (m_sectionStack.size() >= 2)
			? static_cast<std::string>(m_sectionStack[1].name) : "";

		std::string impl, benchName;
		if (m_sectionStack.size() >= 3) {
			// Scaling benchmark: impl is a nested section, benchmark name is just N
			impl      = static_cast<std::string>(m_sectionStack[2].name);
			benchName = TrimRight(static_cast<std::string>(stats.info.name));
		} else {
			// Main benchmark: impl is embedded in the benchmark name
			ParseBenchName(static_cast<std::string>(stats.info.name), benchName, impl);
		}

		m_stream
			<< CsvQuote(currentTestCase) << ','
			<< CsvQuote(workload)        << ','
			<< CsvQuote(impl)            << ','
			<< CsvQuote(benchName)       << ','
			<< meanNs   << ','
			<< iters    << ','
			<< samples  << '\n';
	}

	void benchmarkFailed(Catch::StringRef benchmarkName) override
	{
		std::string workload = (m_sectionStack.size() >= 2)
			? static_cast<std::string>(m_sectionStack[1].name) : "";
		std::string impl = (m_sectionStack.size() >= 3)
			? static_cast<std::string>(m_sectionStack[2].name) : "";

		m_stream
			<< CsvQuote(currentTestCase) << ','
			<< CsvQuote(workload)        << ','
			<< CsvQuote(impl)            << ','
			<< CsvQuote(static_cast<std::string>(benchmarkName))
			<< ",FAILED,,\n";
	}
};

} // anonymous namespace

CATCH_REGISTER_REPORTER("csv", CsvBenchReporter)
