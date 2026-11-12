#include "signaling-client.hpp"

#include "util/log.hpp"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <cstdlib>

namespace qc::vdo {

namespace {
constexpr int kInitialReconnectDelayMs = 1000;
constexpr int kMaxReconnectDelayMs = 30000;

/** Corporate networks often only allow outbound traffic through an HTTP proxy. */
std::optional<rtc::ProxyServer> proxyFromEnvironment()
{
	for (const char *name : {"HTTPS_PROXY", "https_proxy"}) {
		const char *value = std::getenv(name);
		if (!value || !*value)
			continue;
		try {
			return rtc::ProxyServer(std::string(value));
		} catch (const std::exception &) {
			return std::nullopt;
		}
	}
	return std::nullopt;
}
} // namespace

std::shared_ptr<SignalingClient> SignalingClient::create(Executor &executor, std::string url, Callbacks callbacks)
{
	return std::shared_ptr<SignalingClient>(new SignalingClient(executor, std::move(url), std::move(callbacks)));
}

SignalingClient::SignalingClient(Executor &executor, std::string url, Callbacks callbacks)
	: executor_(executor),
	  url_(std::move(url)),
	  callbacks_(std::move(callbacks))
{
}

SignalingClient::~SignalingClient()
{
	if (socket_) {
		socket_->resetCallbacks();
		socket_->close();
	}
}

void SignalingClient::connect()
{
	closedByUser_ = false;
	attempt_ = 0;
	openSocket();
}

void SignalingClient::close()
{
	closedByUser_ = true;
	open_ = false;
	++generation_;
	executor_.cancel(reconnectTimer_);
	reconnectTimer_ = 0;
	if (socket_) {
		socket_->resetCallbacks();
		socket_->close();
		socket_.reset();
	}
}

void SignalingClient::openSocket()
{
	if (closedByUser_)
		return;

	if (socket_) {
		socket_->resetCallbacks();
		socket_->close();
		socket_.reset();
	}
	open_ = false;
	const uint64_t generation = ++generation_;

	rtc::WebSocket::Configuration config;
	config.connectionTimeout = std::chrono::seconds(8);
	// Keeps NATs and proxies from dropping an idle signaling connection during a quiet show.
	config.pingInterval = std::chrono::seconds(20);
	config.maxOutstandingPings = 3;
	config.proxyServer = proxyFromEnvironment();

	std::weak_ptr<SignalingClient> weak = weak_from_this();
	Executor *executor = &executor_;

	try {
		socket_ = std::make_shared<rtc::WebSocket>(config);

		socket_->onOpen([weak, executor, generation] {
			executor->post([weak, generation] {
				auto self = weak.lock();
				if (!self || self->generation_ != generation)
					return;
				self->open_ = true;
				self->attempt_ = 0;
				QC_LOG_INFO("signaling connected");
				if (self->callbacks_.onOpen)
					self->callbacks_.onOpen();
			});
		});

		socket_->onClosed([weak, executor, generation] {
			executor->post([weak, generation] {
				if (auto self = weak.lock())
					self->handleClosed(generation, "connection closed");
			});
		});

		socket_->onError([weak, executor, generation](std::string error) {
			executor->post([weak, generation, error = std::move(error)] {
				if (auto self = weak.lock())
					self->handleClosed(generation, error);
			});
		});

		socket_->onMessage([weak, executor, generation](rtc::message_variant data) {
			if (!std::holds_alternative<std::string>(data))
				return; // the protocol is JSON text only
			executor->post([weak, generation, text = std::get<std::string>(std::move(data))] {
				auto self = weak.lock();
				if (!self || self->generation_ != generation)
					return;
				nlohmann::json message = nlohmann::json::parse(text, nullptr, false);
				if (message.is_discarded()) {
					QC_LOG_DEBUG("signaling: dropped malformed message");
					return;
				}
				if (self->callbacks_.onMessage)
					self->callbacks_.onMessage(message);
			});
		});

		socket_->open(url_);
	} catch (const std::exception &error) {
		QC_LOG_WARN("signaling: could not open socket: %s", error.what());
		socket_.reset();
		scheduleReconnect(error.what());
	}
}

void SignalingClient::handleClosed(uint64_t generation, const std::string &reason)
{
	if (generation != generation_ || closedByUser_)
		return;
	// Invalidate this socket so the paired onClosed/onError callback is ignored.
	++generation_;
	open_ = false;
	scheduleReconnect(reason);
}

void SignalingClient::scheduleReconnect(const std::string &reason)
{
	if (closedByUser_)
		return;

	int delay = kInitialReconnectDelayMs;
	for (int i = 0; i < attempt_ && delay < kMaxReconnectDelayMs; ++i)
		delay *= 2;
	delay = std::min(delay, kMaxReconnectDelayMs);
	delay = std::max(delay, minimumReconnectDelayMs_);
	minimumReconnectDelayMs_ = 0;
	++attempt_;

	QC_LOG_WARN("signaling disconnected (%s); retrying in %d ms", reason.c_str(), delay);
	if (callbacks_.onClosed)
		callbacks_.onClosed(reason, delay);

	std::weak_ptr<SignalingClient> weak = weak_from_this();
	executor_.cancel(reconnectTimer_);
	reconnectTimer_ = executor_.postDelayed(std::chrono::milliseconds(delay), [weak] {
		if (auto self = weak.lock())
			self->openSocket();
	});
}

bool SignalingClient::send(const nlohmann::json &message)
{
	if (!socket_ || !open_)
		return false;
	try {
		return socket_->send(message.dump());
	} catch (const std::exception &error) {
		QC_LOG_WARN("signaling: send failed: %s", error.what());
		return false;
	}
}

} // namespace qc::vdo
