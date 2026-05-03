#ifndef AUDIO_BUFFER_H
#define AUDIO_BUFFER_H

#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <atomic>
#include <iostream>
#include <algorithm>
#include <map>
#include <cmath>
#include <random>

namespace audio {

// Audio configuration constants
constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 1;
constexpr int FRAME_DURATION_MS = 20;
constexpr int SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_DURATION_MS / 1000;  // 960 samples
constexpr int MAX_JITTER_MS = 400;
constexpr int MAX_JITTER_SAMPLES = SAMPLE_RATE * MAX_JITTER_MS / 1000;
constexpr int TARGET_BUFFER_FRAMES = 12;
constexpr int MAX_BUFFER_FRAMES = 30;

// VAD (Voice Activity Detection) thresholds
constexpr float SPEECH_THRESHOLD = 500.0f;   // Energy above this = speaking
constexpr float SILENCE_THRESHOLD = 100.0f;  // Energy below this = silent
constexpr float COMFORT_NOISE_LEVEL = 50.0f; // Very low background noise level

// GCC-PHAT configuration
constexpr int GCC_FFT_SIZE = 1024;  // Power of 2, padded from 960 samples
constexpr int MAX_REASONABLE_DELAY_MS = 50;
constexpr int MAX_REASONABLE_DELAY_SAMPLES = SAMPLE_RATE * MAX_REASONABLE_DELAY_MS / 1000;

/**
 * Audio packet received from a client
 */
struct AudioPacket {
    std::vector<int16_t> samples;
    double client_timestamp;      // Client's NTP timestamp
    std::chrono::steady_clock::time_point server_receive_time;
    
    AudioPacket() : client_timestamp(0) {}
    
    AudioPacket(const std::vector<int16_t>& s, double ts)
        : samples(s)
        , client_timestamp(ts)
        , server_receive_time(std::chrono::steady_clock::now()) {}
    
    // Calculate latency in milliseconds
    double get_latency_ms() const {
        auto now = std::chrono::steady_clock::now();
        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(now - server_receive_time);
        return static_cast<double>(latency.count());
    }
};

/**
 * ClientSession - Manages audio buffer for a single client
 * Includes VAD (Voice Activity Detection) and delay compensation
 */
class ClientSession {
public:
    explicit ClientSession(const std::string& client_id);
    ~ClientSession();
    
    // Basic audio buffer operations
    void add_audio(const std::vector<int16_t>& samples, double timestamp);
    std::vector<int16_t> get_next_frame();
    std::vector<int16_t> peek_next_frame() const;
    void consume_next_frame();
    bool has_audio() const;
    size_t get_buffer_size() const;
    
    // Statistics
    double get_average_latency() const;
    size_t get_total_packets() const;
    size_t get_dropped_packets() const;
    void reset();
    std::string get_id() const { return client_id_; }
    
    // VAD (Voice Activity Detection)
    bool is_speaking() const { return is_speaking_.load(); }
    float get_energy_level() const { return current_energy_.load(); }
    static float get_speech_threshold() { return SPEECH_THRESHOLD; }
    
    // Delay compensation
    void set_delay_compensation(int delay_samples);
    int get_delay_compensation() const { return delay_compensation_; }
    std::vector<int16_t> get_next_frame_compensated();
    
    // Access for GCC processing
    std::vector<int16_t> get_recent_samples(size_t num_samples) const;
    
private:
    const std::string client_id_;
    mutable std::mutex mutex_;
    std::deque<AudioPacket> buffer_;
    std::atomic<size_t> total_packets_{0};
    std::atomic<size_t> dropped_packets_{0};
    std::atomic<double> total_latency_{0.0};
    
    // VAD state
    std::atomic<float> current_energy_{0.0f};
    std::atomic<bool> is_speaking_{false};
    
    // Delay compensation
    int delay_compensation_{0};
    
    void cleanup_old_packets();
    void adjust_buffer_size();
    void update_statistics(const AudioPacket& packet);
    float calculate_energy(const std::vector<int16_t>& samples) const;
};

/**
 * Statistics for a room
 */
struct RoomStats {
    size_t total_clients = 0;
    size_t active_speakers = 0;
    double average_latency_ms = 0.0;
    size_t total_packets_processed = 0;
    size_t total_dropped_packets = 0;
    
    void print() const {
        std::cout << "[RoomStats] Clients: " << total_clients
                  << ", Active: " << active_speakers
                  << ", Avg Latency: " << average_latency_ms << "ms"
                  << ", Packets: " << total_packets_processed
                  << ", Dropped: " << total_dropped_packets << std::endl;
    }
};

/**
 * RoomAudioManager - Manages all clients in a room
 * Integrates VAD to only mix active speakers
 */
class RoomAudioManager {
public:
    explicit RoomAudioManager(const std::string& room_id);
    ~RoomAudioManager();
    
    // Client management
    void add_client(const std::string& client_id, std::shared_ptr<ClientSession> session);
    void remove_client(const std::string& client_id);
    std::shared_ptr<ClientSession> get_client(const std::string& client_id);
    bool has_clients() const;
    size_t get_client_count() const;
    
    // Audio mixing with VAD (only mixes active speakers)
    std::vector<int16_t> get_mixed_audio(size_t samples_needed = SAMPLES_PER_FRAME);
    
     // Get all clients (returns a copy for thread safety)
    std::map<std::string, std::shared_ptr<ClientSession>> get_all_clients() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return clients_;
    }

    // Statistics
    RoomStats get_statistics() const;
    std::string get_room_id() const { return room_id_; }
    
    // Reference client for GCC
    void set_reference_client(const std::string& client_id);
    std::string get_reference_client() const { return reference_client_; }
    
private:
    const std::string room_id_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ClientSession>> clients_;
    std::atomic<size_t> total_frames_mixed_{0};
    std::string reference_client_;
    std::chrono::steady_clock::time_point last_alignment_time_;
    
    // Random number generator for comfort noise
    mutable std::mt19937 rng_;
    mutable std::uniform_int_distribution<int16_t> noise_dist_;
    
    std::vector<int16_t> mix_audio_streams(const std::vector<std::vector<int16_t>>& streams) const;
    int16_t soft_clip(int32_t sample) const;
    std::vector<int16_t> generate_comfort_noise(size_t num_samples) const;
};

} // namespace audio

#endif // AUDIO_BUFFER_H