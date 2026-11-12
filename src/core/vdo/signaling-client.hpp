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

#pragma once

#include "util/executor.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace rtc {
class WebSocket;
}

namespace qc::vdo {

/**
 * WebSocket connection to the signaling server with automatic reconnection.
 *
 * All callbacks are delivered on the supplied Executor, and all public methods must be called on it. Messages sent
 * while disconnected are dropped: the owner re-announces itself (joinroom/seed/play) from `onOpen`.
 */
class SignalingClient : public std::enable_shared_from_this<SignalingClient> {
public:
	struct Callbacks {
		std::function<void()> onOpen;
		/** `reconnectInMs` is negative when no reconnection will be attempted. */
		std::function<void(const std::string &reason, int reconnectInMs)> onClosed;
		std::function<void(const nlohmann::json &)> onMessage;
	};

	static std::shared_ptr<SignalingClient> create(Executor &executor, std::string url, Callbacks callbacks);
	~SignalingClient();

	void connect();
	/** Closes the socket and stops reconnecting. No further callbacks are delivered. */
	void close();
	bool isOpen() const { return open_; }
	bool send(const nlohmann::json &message);

	/** Delays the next reconnection attempt (used when the server says it is busy). */
	void deferReconnect(int minimumDelayMs) { minimumReconnectDelayMs_ = minimumDelayMs; }

private:
	SignalingClient(Executor &executor, std::string url, Callbacks callbacks);

	void openSocket();
	void handleClosed(uint64_t generation, const std::string &reason);
	void scheduleReconnect(const std::string &reason);

	Executor &executor_;
	std::string url_;
	Callbacks callbacks_;

	std::shared_ptr<rtc::WebSocket> socket_;
	/** Bumped for every socket so callbacks from a superseded socket are ignored. */
	uint64_t generation_ = 0;
	bool open_ = false;
	bool closedByUser_ = false;
	int attempt_ = 0;
	int minimumReconnectDelayMs_ = 0;
	Executor::TimerId reconnectTimer_ = 0;
};

} // namespace qc::vdo
