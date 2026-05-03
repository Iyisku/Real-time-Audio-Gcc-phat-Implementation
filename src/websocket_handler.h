#ifndef WEBSOCKET_HANDLER_H
#define WEBSOCKET_HANDLER_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <memory>
#include <set>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>

// Forward declarations
namespace audio {
    class ClientSession;
    class RoomAudioManager;
    class DelayEstimator;
}

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class ClientConnection {
public:
    explicit ClientConnection(std::shared_ptr<websocket::stream<tcp::socket>> ws)
        : ws_(std::move(ws)) {}

    bool send_text(const std::string& message) {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (!ws_) return false;

        boost::system::error_code ec;
        ws_->text(true);
        ws_->write(boost::asio::buffer(message), ec);
        if (ec) {
            std::cerr << "[WebSocket] Send error: " << ec.message() << std::endl;
            return false;
        }
        return true;
    }

    bool send_binary(const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (!ws_) return false;

        boost::system::error_code ec;
        ws_->write(boost::asio::buffer(data), ec);
        if (ec) {
            std::cerr << "[WebSocket] Send binary error: " << ec.message() << std::endl;
            return false;
        }
        return true;
    }

    bool read_message(beast::flat_buffer& buffer, std::string& out_message,
                      boost::system::error_code& ec) {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        ws_->read(buffer, ec);
        if (ec) return false;

        out_message = beast::buffers_to_string(buffer.data());
        return true;
    }

    std::shared_ptr<websocket::stream<tcp::socket>> get_stream() const {
        return ws_;
    }
    
    void close() {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        if (ws_) {
            boost::system::error_code ec;
            ws_->close(websocket::close_code::normal, ec);
        }
    }

private:
    std::shared_ptr<websocket::stream<tcp::socket>> ws_;
    mutable std::mutex stream_mutex_;
};

// Global state (managed with mutexes for thread safety)
extern std::map<std::string, std::set<std::shared_ptr<ClientConnection>>> rooms;
extern std::mutex rooms_mutex;

extern std::map<std::string, std::shared_ptr<audio::RoomAudioManager>> audio_managers;
extern std::mutex audio_managers_mutex;

extern std::map<std::string, std::shared_ptr<audio::DelayEstimator>> room_delay_estimators;
extern std::mutex delay_estimator_mutex;

// WebSocket handler function
void handle_websocket(tcp::socket socket, const http::request<http::string_body>& req);

// Periodic audio mixer thread (for cross-room distribution)
void start_audio_mixer();

// Periodic alignment thread for GCC-PHAT
void start_periodic_alignment();

// Align all streams in a room using GCC-PHAT
void align_room_streams(const std::string& room_id);

#endif // WEBSOCKET_HANDLER_H