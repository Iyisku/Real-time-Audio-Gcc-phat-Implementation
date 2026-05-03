#include "websocket_handler.h"
#include "http_handler.h"
#include "audio_buffer.h"
#include "gcc_phat.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Global state
std::map<std::string, std::set<std::shared_ptr<ClientConnection>>> rooms;
std::mutex rooms_mutex;

std::map<std::string, std::shared_ptr<audio::RoomAudioManager>> audio_managers;
std::mutex audio_managers_mutex;

std::map<std::string, std::shared_ptr<audio::DelayEstimator>> room_delay_estimators;
std::mutex delay_estimator_mutex;

std::atomic<int> g_client_counter{0};

/**
 * Parse audio message from JSON
 */
bool parse_audio_message(const std::string& message, 
                         std::vector<int16_t>& samples, 
                         double& timestamp) {
    try {
        auto json_data = json::parse(message);
        
        if (json_data.contains("type") && json_data["type"] == "audio") {
            if (json_data.contains("samples") && json_data["samples"].is_array()) {
                samples.clear();
                samples.reserve(json_data["samples"].size());
                for (const auto& sample : json_data["samples"]) {
                    samples.push_back(sample.get<int16_t>());
                }
            }
            
            if (json_data.contains("timestamp")) {
                timestamp = json_data["timestamp"].get<double>();
            }
            
            return !samples.empty();
        }
    } catch (const json::exception& e) {
        std::cerr << "[WebSocket] JSON parse error: " << e.what() << std::endl;
    }
    
    return false;
}

/**
 * Broadcast message to all clients in room
 */
void broadcast_to_room(const std::string& room, 
                       const std::string& message,
                       std::shared_ptr<ClientConnection> sender = nullptr) {
    std::lock_guard<std::mutex> lock(rooms_mutex);
    auto it = rooms.find(room);
    if (it != rooms.end()) {
        for (auto& conn : it->second) {
            if (conn != sender) {
                conn->send_text(message);
            }
        }
    }
}

/**
 * Broadcast mixed audio to all clients in room using NTP timestamps for scheduled playback
 */
void broadcast_audio_to_room(const std::string& room_id,
                             const std::vector<int16_t>& mixed_audio,
                             double ntp_timestamp,
                             std::shared_ptr<ClientConnection> sender = nullptr) {
    std::lock_guard<std::mutex> lock(rooms_mutex);
    auto it = rooms.find(room_id);
    if (it != rooms.end()) {
        // Create audio packet with NTP playout timestamp
        json audio_packet = {
            {"type", "audio"},
            {"samples", mixed_audio},
            {"play_at", ntp_timestamp},  // Server NTP time when client should play
            {"sample_rate", audio::SAMPLE_RATE},
            {"frame_duration_ms", audio::FRAME_DURATION_MS}
        };
        
        std::string message = audio_packet.dump();
        
        for (auto& conn : it->second) {
            if (conn != sender) {
                conn->send_text(message);
            }
        }
    }
}

/**
 * Align all streams in a room using GCC-PHAT
 * This runs periodically to adjust for clock drift
 */
/**
 * Align all streams in a room using GCC-PHAT
 * This runs periodically to adjust for clock drift
 */
void align_room_streams(const std::string& room_id) {
    std::lock_guard<std::mutex> audio_lock(audio_managers_mutex);
    
    auto it = audio_managers.find(room_id);
    if (it == audio_managers.end()) return;
    
    auto& manager = it->second;
    if (!manager || manager->get_client_count() < 2) return;
    
    // Get or create delay estimator for this room
    std::lock_guard<std::mutex> delay_lock(delay_estimator_mutex);
    auto& estimator = room_delay_estimators[room_id];
    if (!estimator) {
        estimator = std::make_shared<audio::DelayEstimator>();
        estimator->initialize(1024, audio::SAMPLE_RATE);
        std::cout << "[GCC-PHAT] Created delay estimator for room: " << room_id << std::endl;
    }
    
    // Get all clients
    auto all_clients = manager->get_all_clients();
    if (all_clients.empty()) return;
    
    // Find the best reference client (highest energy = currently speaking)
    std::string reference_id;
    float max_energy = 0;
    std::vector<int16_t> reference_audio;
    
    for (auto& [client_id, session] : all_clients) {
        if (session && session->has_audio()) {
            float energy = session->get_energy_level();
            if (energy > max_energy && energy > audio::SPEECH_THRESHOLD) {
                max_energy = energy;
                reference_id = client_id;
                reference_audio = session->get_recent_samples(1024);
            }
        }
    }
    
    // If no active speaker, use first client with audio
    if (reference_id.empty()) {
        for (auto& [client_id, session] : all_clients) {
            if (session && session->has_audio()) {
                reference_id = client_id;
                reference_audio = session->get_recent_samples(1024);
                break;
            }
        }
    }
    
    if (reference_id.empty() || reference_audio.empty()) {
        return;
    }
    
    // Set reference
    estimator->set_reference(reference_audio);
    std::cout << "[GCC-PHAT] Room '" << room_id << "': Reference client = " << reference_id 
              << " (energy: " << max_energy << ")" << std::endl;
    
    // Estimate delay for each other client and apply compensation
    int delays_applied = 0;
    for (auto& [client_id, session] : all_clients) {
        if (client_id == reference_id) {
            session->set_delay_compensation(0);  // Reference has zero delay
            continue;
        }
        
        if (session && session->has_audio()) {
            auto client_audio = session->get_recent_samples(1024);
            if (!client_audio.empty()) {
                auto result = estimator->estimate_delay(client_audio);
                
                if (result.valid && result.confidence > 0.4f) {
                    // Apply the delay compensation
                    session->set_delay_compensation(result.delay_samples);
                    delays_applied++;
                    
                    std::cout << "[GCC-PHAT] Room '" << room_id 
                              << "': " << client_id << " delay = " 
                              << result.delay_samples << " samples ("
                              << (result.delay_samples * 1000.0f / audio::SAMPLE_RATE) << " ms)"
                              << " confidence: " << result.confidence << std::endl;
                }
            }
        }
    }
    
    if (delays_applied > 0) {
        std::cout << "[GCC-PHAT] Room '" << room_id << "': Applied " << delays_applied 
                  << " delay compensations" << std::endl;
    }
}
/**
 * Start periodic audio mixing for cross-room distribution
 * Now with NTP timestamp tagging for scheduled playback
 */
void start_audio_mixer() {
    std::thread mixer_thread([]() {
        std::cout << "[AudioMixer] Started (runs every " 
                  << audio::FRAME_DURATION_MS << "ms)" << std::endl;
        
        auto get_ntp_time_ms = []() -> double {
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        
        auto next_frame_time = std::chrono::steady_clock::now();
        const auto frame_duration = std::chrono::milliseconds(audio::FRAME_DURATION_MS);
        
        while (true) {
            next_frame_time += frame_duration;
            
            // Get current NTP timestamp for this frame
            double current_ntp_time = get_ntp_time_ms();
            
            // Mix audio for each room
            {
                std::lock_guard<std::mutex> lock(audio_managers_mutex);
                for (auto& [room_id, manager] : audio_managers) {
                    if (manager && manager->has_clients()) {
                        std::vector<int16_t> mixed_audio = manager->get_mixed_audio(audio::SAMPLES_PER_FRAME);
                        
                        if (!mixed_audio.empty()) {
                            broadcast_audio_to_room(room_id, mixed_audio, current_ntp_time);
                        }
                    }
                }
            }
            
            // Sleep until next frame time
            std::this_thread::sleep_until(next_frame_time);
        }
    });
    mixer_thread.detach();
}

/**
 * Start periodic alignment thread for all rooms
 * Runs GCC-PHAT every 2 seconds to adjust for clock drift
 */
void start_periodic_alignment() {
    std::thread alignment_thread([]() {
        std::cout << "[Alignment] Periodic GCC-PHAT alignment started (every 2 seconds)" << std::endl;
        
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
            std::lock_guard<std::mutex> lock(rooms_mutex);
            for (const auto& [room_id, connections] : rooms) {
                if (connections.size() >= 2) {
                    align_room_streams(room_id);
                }
            }
        }
    });
    alignment_thread.detach();
}

/**
 * Handle WebSocket connection with GCC-PHAT delay compensation
 */
void handle_websocket(tcp::socket socket, const http::request<http::string_body>& req) {
    std::string target(req.target());
    
    // Extract room from query string
    std::string room = "Room_A";
    size_t pos = target.find("room=");
    if (pos != std::string::npos) {
        size_t start = pos + 5;
        size_t end = target.find_first_of("&?#", start);
        if (end == std::string::npos) end = target.length();
        room = target.substr(start, end - start);
        if (room.empty()) room = "Room_A";
    }
    
    // URL decode room name (simple version)
    size_t decode_pos = 0;
    while ((decode_pos = room.find('%', decode_pos)) != std::string::npos) {
        if (decode_pos + 2 < room.length()) {
            std::string hex = room.substr(decode_pos + 1, 2);
            char decoded = static_cast<char>(std::stoi(hex, nullptr, 16));
            room.replace(decode_pos, 3, 1, decoded);
        }
        decode_pos++;
    }
    
    // Generate unique client ID
    std::string client_id = "client_" + std::to_string(++g_client_counter);
    
    std::cout << "[WebSocket] New connection - Room: " << room 
              << ", Client: " << client_id << std::endl;
    
    // Create WebSocket stream
    auto ws_ptr = std::make_shared<websocket::stream<tcp::socket>>(std::move(socket));
    auto connection = std::make_shared<ClientConnection>(ws_ptr);
    
    try {
        // Accept WebSocket upgrade
        ws_ptr->accept(req);
        std::cout << "[WebSocket] Accepted - " << client_id << " joined " << room << std::endl;
        
        // Create audio session for this client
        auto audio_session = std::make_shared<audio::ClientSession>(client_id);
        
        // Get or create room audio manager
        std::shared_ptr<audio::RoomAudioManager> room_manager;
        {
            std::lock_guard<std::mutex> lock(audio_managers_mutex);
            auto it = audio_managers.find(room);
            if (it == audio_managers.end()) {
                room_manager = std::make_shared<audio::RoomAudioManager>(room);
                audio_managers[room] = room_manager;
            } else {
                room_manager = it->second;
            }
            room_manager->add_client(client_id, audio_session);
        }
        
        // Add to WebSocket room
        {
            std::lock_guard<std::mutex> lock(rooms_mutex);
            rooms[room].insert(connection);
        }
        
        // Send welcome message with configuration
        json welcome = {
            {"type", "system"},
            {"message", "Connected to room " + room},
            {"client_id", client_id},
            {"sample_rate", audio::SAMPLE_RATE},
            {"frame_duration_ms", audio::FRAME_DURATION_MS},
            {"gcc_phat_enabled", true}
        };
        connection->send_text(welcome.dump());
        
        // Handle incoming messages
        std::thread([connection, room, client_id, audio_session]() {
            try {
                beast::flat_buffer buffer;
                
                while (true) {
                    buffer.consume(buffer.size());
                    std::string message;
                    boost::system::error_code ec;
                    if (!connection->read_message(buffer, message, ec)) {
                        if (ec && ec != websocket::error::closed) {
                            std::cerr << "[WebSocket] Error for " << client_id << ": " 
                                      << ec.message() << std::endl;
                        }
                        break;
                    }
                    
                    // Parse message
                    std::vector<int16_t> samples;
                    double timestamp = 0;
                    
                    if (parse_audio_message(message, samples, timestamp)) {
                        // Audio message - add to jitter buffer
                        // GCC-PHAT delay compensation will be applied during mixing
                        audio_session->add_audio(samples, timestamp);
                        
                        // Log occasionally
                        static std::atomic<int> packet_counter{0};
                        if (++packet_counter % 100 == 0) {
                            std::cout << "[WebSocket] " << client_id 
                                      << ": Received " << packet_counter.load() 
                                      << " audio packets, buffer: " 
                                      << audio_session->get_buffer_size() << std::endl;
                        }
                    } else {
                        // Text message - broadcast to room
                        std::cout << "[WebSocket] Text from " << client_id << ": " 
                                  << message.substr(0, 100) << std::endl;
                        broadcast_to_room(room, message, connection);
                    }
                }
            } catch (const beast::system_error& se) {
                if (se.code() != websocket::error::closed) {
                    std::cerr << "[WebSocket] Error for " << client_id << ": " 
                              << se.what() << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "[WebSocket] Exception for " << client_id << ": " 
                          << e.what() << std::endl;
            }
            
            // Cleanup on disconnect
            {
                std::lock_guard<std::mutex> lock(audio_managers_mutex);
                auto it = audio_managers.find(room);
                if (it != audio_managers.end()) {
                    it->second->remove_client(client_id);
                    if (!it->second->has_clients()) {
                        audio_managers.erase(it);
                        
                        // Clean up delay estimator for empty room
                        std::lock_guard<std::mutex> delay_lock(delay_estimator_mutex);
                        room_delay_estimators.erase(room);
                    }
                }
            }
            
            // Remove from WebSocket room
            {
                std::lock_guard<std::mutex> lock(rooms_mutex);
                auto it = rooms.find(room);
                if (it != rooms.end()) {
                    it->second.erase(connection);
                    if (it->second.empty()) {
                        rooms.erase(it);
                    }
                }
            }
            
            std::cout << "[WebSocket] Disconnected - " << client_id << " left " << room << std::endl;
        }).detach();
        
    } catch (const beast::system_error& se) {
        std::cerr << "[WebSocket] Accept error: " << se.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[WebSocket] Exception: " << e.what() << std::endl;
    }
}