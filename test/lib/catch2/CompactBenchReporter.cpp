/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

std::string FormatDuration(double ns)
{
	char buf[32];
	if (ns < 1000.0)
		std::snprintf(buf, sizeof(buf), "%7.2f ns", ns);
	else if (ns < 1'000'000.0)
		std::snprintf(buf, sizeof(buf), "%7.2f us", ns / 1'000.0);
	else if (ns < 1'000'000'000.0)
		std::snprintf(buf, sizeof(buf), "%7.2f ms", ns / 1'000'000.0);
	else
		std::snprintf(buf, sizeof(buf), "%7.2f s ", ns / 1'000'000'000.0);
	return buf;
}

class CompactBenchReporter final : public Catch::StreamingReporterBase {
	std::string currentTestCase;

public:
	using StreamingReporterBase::StreamingReporterBase;

	static std::string getDescription()
	{
		return "Compact benchmark reporter showing mean, iterations, and total time";
	}

	void testCaseStarting(Catch::TestCaseInfo const& info) override
	{
		StreamingReporterBase::testCaseStarting(info);
		currentTestCase = info.name;
	}

	void sectionStarting(Catch::SectionInfo const& info) override
	{
		StreamingReporterBase::sectionStarting(info);

		// Skip the root section (same name as test case)
		if (m_sectionStack.size() <= 1)
			return;

		// Section header: ── TestCase / Section ──────────────
		const std::string title = currentTestCase + " / " + info.name;
		m_stream << "\n"
		         << "\xe2\x94\x80\xe2\x94\x80 " << title << " ";

		const int remaining = std::max(0, 70 - 4 - static_cast<int>(title.size()));
		for (int i = 0; i < remaining; ++i)
			m_stream << "\xe2\x94\x80";
		m_stream << "\n\n";

		// Column headers and separator
		char header[128];
		std::snprintf(header, sizeof(header), "  %-21s  %10s  %10s    %10s\n",
			"Benchmark", "Mean", "Iterations", "Total Time");
		m_stream << header;
		m_stream << "  \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
		            "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
		            "\xe2\x94\x80"
		            "  "
		            "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
		            "  "
		            "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
		            "    "
		            "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
		            "\n";
	}

	void benchmarkEnded(Catch::BenchmarkStats<> const& stats) override
	{
		const double meanNs = stats.mean.point.count();
		const int64_t iterations =
			static_cast<int64_t>(stats.info.iterations) * stats.info.samples;
		const double totalNs = meanNs * iterations;

		char buf[256];
		std::snprintf(buf, sizeof(buf), "  %-21s  %10s  %10lld    %10s\n",
			stats.info.name.c_str(),
			FormatDuration(meanNs).c_str(),
			static_cast<long long>(iterations),
			FormatDuration(totalNs).c_str());
		m_stream << buf;
	}

	void benchmarkFailed(Catch::StringRef benchmarkName) override
	{
		m_stream << "  " << benchmarkName << "  FAILED\n";
	}

	void testRunEnded(Catch::TestRunStats const& stats) override
	{
		m_stream << "\n";
		const auto& totals = stats.totals;
		if (totals.assertions.failed == 0)
			m_stream << "All tests passed";
		else
			m_stream << "FAILED (failures: " << totals.assertions.failed << ")";
		m_stream << " (" << totals.testCases.total() << " test cases)\n";

		StreamingReporterBase::testRunEnded(stats);
	}
};

} // anonymous namespace

CATCH_REGISTER_REPORTER("compact-bench", CompactBenchReporter)
