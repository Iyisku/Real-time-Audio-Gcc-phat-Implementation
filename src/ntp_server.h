#ifndef NTP_SERVER_H
#define NTP_SERVER_H

#include <boost/asio.hpp>
#include <cstdint>
#include <memory>
#include <random>

namespace net = boost::asio;

// NTP Protocol Constants
constexpr int NTP_PORT = 123;
constexpr uint32_t UNIX_EPOCH_OFFSET = 2208988800UL;

class NTPServer {
public:
    NTPServer(net::io_context& ioc);
    ~NTPServer();
    
private:
    void receive();
    void handle_ntp_request(const char* data, size_t length, 
                           const net::ip::udp::endpoint& client);
    std::pair<uint32_t, uint32_t> get_ntp_timestamp();
    
    net::io_context& ioc_;
    net::ip::udp::socket socket_;
    std::mt19937 rng_;
    std::uniform_int_distribution<uint32_t> dist_;
};

#endif // NTP_SERVER_H