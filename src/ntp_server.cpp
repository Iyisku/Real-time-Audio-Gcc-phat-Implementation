#include "ntp_server.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <arpa/inet.h>  // For ntohl, htonl

NTPServer::NTPServer(net::io_context& ioc) 
    : ioc_(ioc), 
      socket_(ioc_, net::ip::udp::endpoint(net::ip::udp::v4(), NTP_PORT)),
      rng_(std::random_device{}()),
      dist_(0, 0xFFFFFFFF) {
    
    std::cout << "[NTP] Server listening on port " << NTP_PORT << std::endl;
    receive();
}

NTPServer::~NTPServer() {
    boost::system::error_code ec;
    socket_.close(ec);
}

std::pair<uint32_t, uint32_t> NTPServer::get_ntp_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration - seconds);
    
    uint32_t secs = static_cast<uint32_t>(seconds.count()) + UNIX_EPOCH_OFFSET;
    // Convert microseconds to NTP fraction (2^32 / 1,000,000)
    uint32_t frac = static_cast<uint32_t>(microseconds.count() * 4294.967296);
    
    return {secs, frac};
}

void NTPServer::receive() {
    auto buffer = std::make_shared<std::array<char, 1024>>();
    auto client_endpoint = std::make_shared<net::ip::udp::endpoint>();
    
    socket_.async_receive_from(
        net::buffer(*buffer), *client_endpoint,
        [this, buffer, client_endpoint](boost::system::error_code ec, size_t bytes_recvd) {
            if (!ec) {
                handle_ntp_request(buffer->data(), bytes_recvd, *client_endpoint);
            } else if (ec != boost::asio::error::operation_aborted) {
                std::cerr << "[NTP] Receive error: " << ec.message() << std::endl;
            }
            receive(); // Listen for next packet
        }
    );
}

void NTPServer::handle_ntp_request(const char* data, size_t length, 
                                   const net::ip::udp::endpoint& client) {
    if (length < 48) {  // NTP packets are typically 48 bytes
        std::cerr << "[NTP] Invalid packet size: " << length << std::endl;
        return;
    }
    
    // Parse client packet (network byte order)
    const uint8_t* pkt = reinterpret_cast<const uint8_t*>(data);
    uint8_t li_vn_mode = pkt[0];
    
    // Check mode (should be 3 for client)
    uint8_t mode = li_vn_mode & 0x07;
    if (mode != 3) {
        // Not a client request, ignore
        return;
    }
    
    // Get client's transmit timestamp (fields 8-11, 12-15 in NTP packet)
    uint32_t client_tx_sec;
    uint32_t client_tx_frac;
    std::memcpy(&client_tx_sec, pkt + 40, 4);
    std::memcpy(&client_tx_frac, pkt + 44, 4);
    
    // Convert from network byte order
    client_tx_sec = ntohl(client_tx_sec);
    client_tx_frac = ntohl(client_tx_frac);
    
    // Get current time
    auto [rx_sec, rx_frac] = get_ntp_timestamp();
    auto [tx_sec, tx_frac] = get_ntp_timestamp();
    
    // Prepare response packet (all fields in network byte order)
    uint8_t response[48] = {0};
    
    // LI, VN, Mode: 0x1C = 0b00011100 (VN=3, Mode=4 - server)
    response[0] = 0x1C;
    response[1] = 2;      // Stratum (2 = secondary reference)
    response[2] = 4;      // Poll interval (16 seconds)
    response[3] = -20;    // Precision (~1 microsecond)
    
    // Root delay, dispersion, reference ID (0 for now)
    // Reference ID = "LOCL" for local clock
    response[12] = 'L';
    response[13] = 'O';
    response[14] = 'C';
    response[15] = 'L';
    
    // Origin timestamp (client's transmit time)
    uint32_t orig_sec_net = htonl(client_tx_sec);
    uint32_t orig_frac_net = htonl(client_tx_frac);
    std::memcpy(response + 24, &orig_sec_net, 4);
    std::memcpy(response + 28, &orig_frac_net, 4);
    
    // Receive timestamp (when server received request)
    uint32_t rx_sec_net = htonl(rx_sec);
    uint32_t rx_frac_net = htonl(rx_frac);
    std::memcpy(response + 32, &rx_sec_net, 4);
    std::memcpy(response + 36, &rx_frac_net, 4);
    
    // Transmit timestamp (when server sends response)
    uint32_t tx_sec_net = htonl(tx_sec);
    uint32_t tx_frac_net = htonl(tx_frac);
    std::memcpy(response + 40, &tx_sec_net, 4);
    std::memcpy(response + 44, &tx_frac_net, 4);
    
    // Send response
    boost::system::error_code ec;
    socket_.send_to(net::buffer(response, 48), client, 0, ec);
    
    if (ec) {
        std::cerr << "[NTP] Send error: " << ec.message() << std::endl;
    } else {
        std::cout << "[NTP] Response sent to " << client.address() << ":" 
                  << client.port() << std::endl;
    }
}