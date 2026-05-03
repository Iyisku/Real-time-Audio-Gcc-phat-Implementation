#include <iostream>
#include <memory>
#include <thread>
#include <signal.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "ntp_server.h"
#include "http_handler.h"
#include "websocket_handler.h"

std::atomic<bool> g_running{true};

void signal_handler(int signal) {
    std::cout << "\n[Main] Received signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
}

int main() {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "🎙️ Speeqr Audio Synchronization Server" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Version: Day 2 - Audio Ingress & Buffer" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "WebSocket server: ws://localhost:8080" << std::endl;
    std::cout << "HTTP server:      http://localhost:8080" << std::endl;
    std::cout << "NTP server:       udp://localhost:1123" << std::endl;
    std::cout << "========================================\n" << std::endl;
    
    try {
        net::io_context ioc{1};
        
        // Start NTP server
        NTPServer ntp_server(ioc);
        
        // HTTP/WebSocket acceptor
        tcp::acceptor acceptor{ioc, tcp::endpoint(tcp::v4(), 8080)};
        
        // Start audio mixer thread for cross-room distribution
        start_audio_mixer();
        
        std::cout << "[Main] All services started!" << std::endl;
        std::cout << "[Main] Waiting for connections...\n" << std::endl;
        
        // Run io_context in separate thread
        std::thread io_thread([&ioc]() {
            ioc.run();
        });
        
        // Main accept loop
        while (g_running) {
            tcp::socket socket{ioc};
            boost::system::error_code ec;
            
            acceptor.accept(socket, ec);
            if (ec) {
                if (g_running) {
                    std::cerr << "[Main] Accept error: " << ec.message() << std::endl;
                }
                continue;
            }
            
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            
            http::read(socket, buffer, req, ec);
            if (ec) {
                std::cerr << "[Main] Read error: " << ec.message() << std::endl;
                continue;
            }
            
            if (websocket::is_upgrade(req)) {
                handle_websocket(std::move(socket), req);
            } else {
                handle_http_request(socket, req);
                socket.shutdown(tcp::socket::shutdown_send, ec);
            }
        }
        
        // Cleanup
        std::cout << "[Main] Shutting down..." << std::endl;
        ioc.stop();
        io_thread.join();
        
    } catch (const std::exception& e) {
        std::cerr << "[Main] Fatal error: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "[Main] Goodbye!" << std::endl;
    return 0;
}