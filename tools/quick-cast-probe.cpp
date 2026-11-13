/*
Quick Cast - remote guests for OBS Studio
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// Developer tool: hosts a show from the command line, or joins someone else's, and reports what arrives.
//
//   quick-cast-probe --room NAME [--password PW] [--seat ID:Name]... [--seconds N]
//                    [--dump DIR] [--return-h264 FILE --return-fps N] [--no-return] [--kick-after N]
//   quick-cast-probe --join LINK [--name NAME] [--seconds N] [--dump DIR] [--return-h264 FILE]
//
// --join takes a seat in the show an invite link leads to, the way the plugin's "Join a show" does. Run one probe
// as the host and another with one of the links it prints to exercise both ends without a browser.
//
// --dump writes each guest's raw video elementary stream to DIR/<seat>.h264|.ivf-less vp8 frames and the Opus
// packet sizes are only counted. --return-h264 loops an Annex-B file as the return feed (one access unit per frame,
// split on access unit delimiters or SPS/IDR/slice starts).

#include "session/guest-service.hpp"
#include "util/log.hpp"
#include "vdo/vdo-service.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace qc;

namespace {

struct CountingSink final : GuestMediaSink {
	std::string name;
	std::ofstream dump;
	std::atomic<uint64_t> videoFrames{0};
	std::atomic<uint64_t> keyframes{0};
	std::atomic<uint64_t> videoBytes{0};
	std::atomic<uint64_t> audioPackets{0};
	std::atomic<uint64_t> lastVideoNs{0};
	std::atomic<uint64_t> lastAudioNs{0};
	std::atomic<int> codec{-1};
	std::atomic<int> stops{0};

	void onVideoFrame(EncodedVideoFrame &&frame) override
	{
		++videoFrames;
		if (frame.keyframe)
			++keyframes;
		videoBytes += frame.data.size();
		lastVideoNs = frame.timestampNs;
		codec = static_cast<int>(frame.codec);
		if (dump.is_open())
			dump.write(reinterpret_cast<const char *>(frame.data.data()),
				   static_cast<std::streamsize>(frame.data.size()));
	}
	void onAudioPacket(EncodedAudioPacket &&packet) override
	{
		++audioPackets;
		lastAudioNs = packet.timestampNs;
	}
	void onMediaStopped() override { ++stops; }
};

struct Listener final : GuestServiceListener {
	GuestService *service = nullptr;
	std::string dumpDir;
	std::mutex mutex;
	std::map<std::string, std::shared_ptr<CountingSink>> sinks;
	std::vector<GuestSeat> created;

	void attach(const GuestSeat &seat)
	{
		auto sink = std::make_shared<CountingSink>();
		sink->name = seat.name;
		if (!dumpDir.empty())
			sink->dump.open(dumpDir + "/" + seat.id + ".video", std::ios::binary);
		{
			std::lock_guard<std::mutex> lock(mutex);
			sinks[seat.id] = sink;
		}
		// Must not be called from the service thread synchronously with itself; setMediaSink handles that.
		service->setMediaSink(seat.id, sink);
	}

	void onServiceState(ServiceState state, const std::string &detail) override
	{
		static const char *names[] = {"stopped", "connecting", "live", "reconnecting", "failed"};
		std::printf("[state] %s %s\n", names[static_cast<int>(state)], detail.c_str());
		std::fflush(stdout);
	}
	void onGuestStatus(const GuestStatus &status) override
	{
		static const char *names[] = {"offline", "connecting", "connected"};
		std::printf("[guest] %s %s name='%s' micMuted=%d camMuted=%d return=%d\n", status.id.c_str(),
			    names[static_cast<int>(status.connection)], status.reportedName.c_str(),
			    status.microphoneMuted ? 1 : 0, status.cameraMuted ? 1 : 0,
			    status.receivingReturnFeed ? 1 : 0);
		std::fflush(stdout);
	}
	void onSeatCreated(const GuestSeat &seat) override
	{
		std::printf("[seat] created %s\n", seat.id.c_str());
		std::fflush(stdout);
		attach(seat);
	}
	void onReturnFeedDemand(bool wanted) override
	{
		std::printf("[return] demand=%d\n", wanted ? 1 : 0);
		std::fflush(stdout);
	}
};

/** Splits an Annex-B stream into access units, starting a new one at each SPS or (failing that) each slice. */
std::vector<std::pair<std::vector<uint8_t>, bool>> loadAccessUnits(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	std::vector<size_t> starts;
	for (size_t i = 0; i + 3 < data.size(); ++i) {
		if (data[i] == 0 && data[i + 1] == 0 && (data[i + 2] == 1 || (data[i + 2] == 0 && data[i + 3] == 1))) {
			starts.push_back(i);
			i += 2;
		}
	}
	std::vector<std::pair<std::vector<uint8_t>, bool>> units;
	std::vector<uint8_t> current;
	bool currentKey = false;
	bool haveSlice = false;
	for (size_t n = 0; n < starts.size(); ++n) {
		const size_t begin = starts[n];
		const size_t end = n + 1 < starts.size() ? starts[n + 1] : data.size();
		size_t header = begin + (data[begin + 2] == 1 ? 3 : 4);
		if (header >= end)
			continue;
		const uint8_t type = data[header] & 0x1F;
		const bool isSlice = type == 1 || type == 5;
		const bool startsUnit = type == 7 || type == 9 || (isSlice && haveSlice);
		if (startsUnit && haveSlice) {
			units.emplace_back(std::move(current), currentKey);
			current.clear();
			currentKey = false;
			haveSlice = false;
		}
		current.insert(current.end(), data.begin() + static_cast<long>(begin),
			       data.begin() + static_cast<long>(end));
		if (type == 5)
			currentKey = true;
		if (isSlice)
			haveSlice = true;
	}
	if (haveSlice)
		units.emplace_back(std::move(current), currentKey);
	return units;
}

} // namespace

int main(int argc, char **argv)
{
	ShowConfig config;
	int seconds = 30;
	std::string dumpDir;
	std::string returnFile;
	int returnFps = 30;
	bool verbose = false;
	int kickAfter = 0;
	std::string joinLink;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		const auto next = [&]() -> std::string {
			return i + 1 < argc ? argv[++i] : "";
		};
		if (arg == "--room")
			config.room = next();
		else if (arg == "--password")
			config.password = next();
		else if (arg == "--seconds")
			seconds = std::stoi(next());
		else if (arg == "--dump")
			dumpDir = next();
		else if (arg == "--return-h264")
			returnFile = next();
		else if (arg == "--return-fps")
			returnFps = std::stoi(next());
		else if (arg == "--no-return")
			config.returnFeedEnabled = false;
		else if (arg == "--vp8")
			config.defaultQuality.preferredCodec = VideoCodec::VP8;
		else if (arg == "--vp9")
			config.defaultQuality.preferredCodec = VideoCodec::VP9;
		else if (arg == "--bitrate")
			config.defaultQuality.maxVideoBitrateKbps = static_cast<uint32_t>(std::stoi(next()));
		else if (arg == "--verbose")
			verbose = true;
		else if (arg == "--join")
			joinLink = next();
		else if (arg == "--name")
			config.hostName = next();
		else if (arg == "--kick-after")
			kickAfter = std::stoi(next());
		else if (arg == "--seat") {
			const std::string value = next();
			const size_t colon = value.find(':');
			config.seats.push_back(GuestSeat{value.substr(0, colon),
							 colon == std::string::npos ? value : value.substr(colon + 1)});
		}
	}
	if (config.room.empty() && joinLink.empty()) {
		std::fprintf(stderr,
			     "usage: quick-cast-probe --room NAME [--password PW] [--seat ID:Name] [--seconds N]\n"
			     "       quick-cast-probe --join LINK [--name NAME] [--seconds N]\n");
		return 2;
	}

	log::setSink([verbose](log::Level level, const std::string &message) {
		if (level == log::Level::Debug && !verbose)
			return;
		static const char *names[] = {"debug", "info", "warn", "error"};
		std::printf("[log:%s] %s\n", names[static_cast<int>(level)], message.c_str());
		std::fflush(stdout);
	});

	vdo::VdoService service;
	Listener listener;
	listener.service = &service;
	listener.dumpDir = dumpDir;
	service.setListener(&listener);

	if (!joinLink.empty()) {
		const auto target = service.parseInviteLink(joinLink);
		if (!target) {
			std::fprintf(stderr, "not an invite link: %s\n", joinLink.c_str());
			return 2;
		}
		config.role = ShowRole::Guest;
		config.room = target->room;
		config.password = target->password;
		config.ownSeatId = target->seatId;
		config.seatPrefix = "joined:";
		config.seats.clear();
		std::printf("[join] room=%s seat=%s\n", config.room.c_str(),
			    config.ownSeatId.empty() ? "(open link)" : config.ownSeatId.c_str());
	} else {
		for (const GuestSeat &seat : config.seats)
			std::printf("[link] %s -> %s\n", seat.name.c_str(), service.inviteLink(config, seat).c_str());
		std::printf("[link] open -> %s\n", service.openInviteLink(config).c_str());
	}
	std::fflush(stdout);

	service.start(config);
	for (const GuestSeat &seat : config.seats)
		listener.attach(seat);

	std::atomic<bool> running{true};
	std::thread returnThread;
	if (!returnFile.empty()) {
		auto units = loadAccessUnits(returnFile);
		std::printf("[return] loaded %zu access units\n", units.size());
		if (!units.empty()) {
			returnThread = std::thread([&service, &running, units = std::move(units), returnFps] {
				const auto start = std::chrono::steady_clock::now();
				// 20 ms of Opus silence (CELT fullband, mono) keeps the audio track alive.
				const uint8_t silence[] = {0xF8, 0xFF, 0xFE};
				uint64_t frame = 0;
				uint64_t audioFrame = 0;
				while (running) {
					const int64_t ptsUs = static_cast<int64_t>(frame * 1'000'000 / returnFps);
					const auto &unit = units[frame % units.size()];
					service.sendReturnVideo(ReturnFeedId::Main, unit.first.data(),
								unit.first.size(), ptsUs, unit.second);
					while (static_cast<int64_t>(audioFrame * 20'000) <= ptsUs) {
						service.sendReturnAudio(silence, sizeof(silence),
									static_cast<int64_t>(audioFrame * 20'000));
						++audioFrame;
					}
					++frame;
					std::this_thread::sleep_until(
						start + std::chrono::microseconds(frame * 1'000'000 / returnFps));
				}
			});
		}
	}

	for (int second = 1; second <= seconds; ++second) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		std::lock_guard<std::mutex> lock(listener.mutex);
		if (kickAfter > 0 && second == kickAfter) {
			for (const auto &[id, sink] : listener.sinks) {
				std::printf("[probe] disconnecting %s\n", id.c_str());
				service.disconnectGuest(id);
			}
		}
		for (const auto &[id, sink] : listener.sinks) {
			GuestMediaStats stats;
			const bool have = service.guestStats(id, stats);
			const double avSkewMs = (static_cast<double>(sink->lastVideoNs.load()) -
						 static_cast<double>(sink->lastAudioNs.load())) /
						1e6;
			std::printf(
				"[stats t=%d] %s codec=%d frames=%llu key=%llu vbytes=%llu audio=%llu kbps=%.0f loss=%.3f "
				"jitter=%.1fms pli=%llu avskew=%.0fms stops=%d\n",
				second, id.c_str(), sink->codec.load(), (unsigned long long)sink->videoFrames.load(),
				(unsigned long long)sink->keyframes.load(), (unsigned long long)sink->videoBytes.load(),
				(unsigned long long)sink->audioPackets.load(), have ? stats.videoBitrateKbps : 0.0,
				have ? stats.videoLoss : 0.0, have ? stats.jitterMs : 0.0,
				(unsigned long long)(have ? stats.keyframeRequests : 0), avSkewMs, sink->stops.load());
		}
		std::fflush(stdout);
	}

	running = false;
	if (returnThread.joinable())
		returnThread.join();
	service.stop();
	std::printf("[done]\n");
	return 0;
}
