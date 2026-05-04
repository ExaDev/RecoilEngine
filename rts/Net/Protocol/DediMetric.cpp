/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "DediMetric.h"

#include "DediMsgHeaders.h"
#include "BaseNetProtocol.h"
#include "NetProtocol.h"
#include "Game/GlobalUnsynced.h"
#include "System/Net/PackPacket.h"
#include "System/Log/ILog.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

bool ValidateName(std::string_view name)
{
	const size_t nameLen = name.size();
	if (nameLen == 0 || nameLen > dedimsg::kMaxMetricNameLen)
		return false;
	for (size_t i = 0; i < nameLen; ++i) {
		const char c = name[i];
		if (c == '\0' || c == '\t' || c == '\n' || c == '"' || c == '\\')
			return false;
	}
	return true;
}

bool Send(uint16_t header, std::string_view name, double value, int teamNum, const char* api)
{
	if (clientNet == nullptr)
		return false;
	if (!ValidateName(name)) {
		LOG_L(L_WARNING, "[%s] invalid name (len=%zu)", api, name.size());
		return false;
	}
	if (!std::isfinite(value)) {
		LOG_L(L_WARNING, "[%s] non-finite value", api);
		return false;
	}
	if (teamNum < -1 || teamNum >= dedimsg::kNoTeam) {
		LOG_L(L_WARNING, "[%s] teamNum out of range (got %d)", api, teamNum);
		return false;
	}

	const uint8_t teamByte = (teamNum < 0) ? dedimsg::kNoTeam : static_cast<uint8_t>(teamNum);

	std::vector<uint8_t> payload;
	payload.reserve(1 + 1 + name.size() + sizeof(double));
	payload.push_back(teamByte);
	payload.push_back(static_cast<uint8_t>(name.size()));
	payload.insert(payload.end(),
		reinterpret_cast<const uint8_t*>(name.data()),
		reinterpret_cast<const uint8_t*>(name.data()) + name.size());
	const uint8_t* vbytes = reinterpret_cast<const uint8_t*>(&value);
	payload.insert(payload.end(), vbytes, vbytes + sizeof(double));

	try {
		clientNet->Send(CBaseNetProtocol::Get().SendDediMsg(
			gu->myPlayerNum, header, payload));
	} catch (const netcode::PackPacketException& ex) {
		LOG_L(L_WARNING, "[%s] packet error: %s", api, ex.what());
		return false;
	}
	return true;
}

} // namespace


namespace dedimetric {

bool Counter(std::string_view name, double value, int teamNum)
{
	return Send(dedimsg::METRIC_COUNTER, name, value, teamNum, "dedimetric::Counter");
}

bool Gauge(std::string_view name, double value, int teamNum)
{
	return Send(dedimsg::METRIC_GAUGE, name, value, teamNum, "dedimetric::Gauge");
}

} // namespace dedimetric
