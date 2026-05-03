#include "audio_buffer.h"
#include <cmath>
#include <cstring>
#include <numeric>

namespace audio {

// ============================================================================
// ClientSession Implementation
// ============================================================================

ClientSession::ClientSession(const std::string& client_id)
    : client_id_(client_id) {
    std::cout << "[ClientSession] Created: " << client_id_ << std::endl;
}

ClientSession::~ClientSession() {
    std::cout << "[ClientSession] Destroyed: " << client_id_
              << " (Packets: " << total_packets_.load()
              << ", Dropped: " << dropped_packets_.load()
              << ", Avg Latency: " << get_average_latency() << "ms)"
              << std::endl;
}

float ClientSession::calculate_energy(const std::vector<int16_t>& samples) const {
    float energy = 0.0f;
    for (auto sample : samples) {
        energy += static_cast<float>(sample) * sample;
    }
    return energy / samples.size();
}

void ClientSession::update_statistics(const AudioPacket& packet) {
    double latency = packet.get_latency_ms();
    total_packets_++;
    double old_val = total_latency_.load();
    while (!total_latency_.compare_exchange_weak(old_val, old_val + latency)) {}
}

void ClientSession::add_audio(const std::vector<int16_t>& samples, double timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (samples.empty()) {
        dropped_packets_++;
        return;
    }
    
    // Calculate energy for VAD
    float energy = calculate_energy(samples);
    current_energy_.store(energy);
    
    // Voice Activity Detection with hysteresis
    if (energy > SPEECH_THRESHOLD) {
        is_speaking_.store(true);
    } else if (energy < SILENCE_THRESHOLD) {
        is_speaking_.store(false);
    }
    // Between thresholds: maintain previous state (reduces toggling)
    
    AudioPacket packet(samples, timestamp);
    update_statistics(packet);
    
    // Insert in timestamp order (handles out-of-order packets)
    auto it = buffer_.begin();
    while (it != buffer_.end() && it->client_timestamp < timestamp) {
        ++it;
    }
    buffer_.insert(it, std::move(packet));
    
    cleanup_old_packets();
    adjust_buffer_size();
    
    // Log VAD state changes occasionally
    if (total_packets_.load() % 100 == 0) {
        std::cout << "[ClientSession] " << client_id_
                  << ": Energy=" << static_cast<int>(energy)
                  << ", Speaking=" << (is_speaking_.load() ? "YES" : "no")
                  << ", Buffer=" << buffer_.size()
                  << ", Dropped=" << dropped_packets_.load()
                  << std::endl;
    }
}

void ClientSession::cleanup_old_packets() {
    auto now = std::chrono::steady_clock::now();
    auto max_age = std::chrono::milliseconds(MAX_JITTER_MS);
    
    size_t removed = 0;
    while (!buffer_.empty()) {
        auto age = now - buffer_.front().server_receive_time;
        if (age > max_age) {
            buffer_.pop_front();
            removed++;
            dropped_packets_++;
        } else {
            break;
        }
    }
    
    if (removed > 0) {
        std::cout << "[ClientSession] " << client_id_ 
                  << ": Cleaned " << removed << " old packets" << std::endl;
    }
}

void ClientSession::adjust_buffer_size() {
    // Dynamic buffer target based on packet loss
    float loss_rate = static_cast<float>(dropped_packets_.load()) / (total_packets_.load() + 1);
    
    int dynamic_target = TARGET_BUFFER_FRAMES;
    if (loss_rate > 0.1) {
        dynamic_target = TARGET_BUFFER_FRAMES + 6;
    } else if (loss_rate > 0.05) {
        dynamic_target = TARGET_BUFFER_FRAMES + 3;
    } else if (loss_rate < 0.02) {
        dynamic_target = TARGET_BUFFER_FRAMES - 2;
    }
    dynamic_target = std::max(6, std::min(MAX_BUFFER_FRAMES - 4, dynamic_target));
    
    if (buffer_.size() > static_cast<size_t>(dynamic_target + 4)) {
        size_t to_drop = buffer_.size() - dynamic_target;
        for (size_t i = 0; i < to_drop; ++i) {
            buffer_.pop_front();
            dropped_packets_++;
        }
        std::cout << "[ClientSession] " << client_id_
                  << ": Adaptive buffer - Target: " << dynamic_target 
                  << ", Dropped: " << to_drop << " packets" << std::endl;
    }
}

std::vector<int16_t> ClientSession::get_next_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (buffer_.empty()) {
        return std::vector<int16_t>(SAMPLES_PER_FRAME, 0);
    }
    
    AudioPacket packet = std::move(buffer_.front());
    buffer_.pop_front();
    
    std::vector<int16_t> result = std::move(packet.samples);
    
    if (result.size() < SAMPLES_PER_FRAME) {
        result.resize(SAMPLES_PER_FRAME, 0);
    } else if (result.size() > SAMPLES_PER_FRAME) {
        result.resize(SAMPLES_PER_FRAME);
    }
    
    return result;
}

std::vector<int16_t> ClientSession::peek_next_frame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (buffer_.empty()) {
        return std::vector<int16_t>(SAMPLES_PER_FRAME, 0);
    }
    
    std::vector<int16_t> result = buffer_.front().samples;
    
    if (result.size() < SAMPLES_PER_FRAME) {
        result.resize(SAMPLES_PER_FRAME, 0);
    } else if (result.size() > SAMPLES_PER_FRAME) {
        result.resize(SAMPLES_PER_FRAME);
    }
    
    return result;
}

void ClientSession::consume_next_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!buffer_.empty()) {
        buffer_.pop_front();
    }
}

std::vector<int16_t> ClientSession::get_recent_samples(size_t num_samples) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<int16_t> result;
    result.reserve(num_samples);
    
    size_t samples_collected = 0;
    
    for (auto it = buffer_.rbegin(); it != buffer_.rend() && samples_collected < num_samples; ++it) {
        const auto& samples = it->samples;
        size_t to_copy = std::min(num_samples - samples_collected, samples.size());
        
        auto start = samples.begin() + (samples.size() - to_copy);
        result.insert(result.end(), start, samples.end());
        samples_collected += to_copy;
    }
    
    if (samples_collected < num_samples) {
        result.insert(result.end(), num_samples - samples_collected, 0);
    }
    
    std::reverse(result.begin(), result.end());
    return result;
}

void ClientSession::set_delay_compensation(int delay_samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    int max_delay = MAX_REASONABLE_DELAY_SAMPLES;
    delay_samples = std::max(-max_delay, std::min(max_delay, delay_samples));
    delay_compensation_ = delay_samples;
    
    if (delay_samples != 0) {
        std::cout << "[ClientSession] " << client_id_ 
                  << ": Delay compensation set to " << delay_samples 
                  << " samples (" << (delay_samples * 1000.0 / SAMPLE_RATE) << " ms)" 
                  << std::endl;
    }
}

std::vector<int16_t> ClientSession::get_next_frame_compensated() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (buffer_.empty()) {
        return std::vector<int16_t>(SAMPLES_PER_FRAME, 0);
    }
    
    AudioPacket packet = std::move(buffer_.front());
    buffer_.pop_front();
    
    std::vector<int16_t> result = std::move(packet.samples);
    
    if (delay_compensation_ > 0) {
        std::vector<int16_t> delayed(result.size() + delay_compensation_, 0);
        std::copy(result.begin(), result.end(), delayed.begin() + delay_compensation_);
        result = std::move(delayed);
    } else if (delay_compensation_ < 0) {
        int skip = std::min(-delay_compensation_, static_cast<int>(result.size()));
        std::vector<int16_t> advanced(result.begin() + skip, result.end());
        result = std::move(advanced);
    }
    
    if (result.size() < SAMPLES_PER_FRAME) {
        result.resize(SAMPLES_PER_FRAME, 0);
    } else if (result.size() > SAMPLES_PER_FRAME) {
        result.resize(SAMPLES_PER_FRAME);
    }
    
    return result;
}

bool ClientSession::has_audio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !buffer_.empty();
}

size_t ClientSession::get_buffer_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

double ClientSession::get_average_latency() const {
    size_t packets = total_packets_.load();
    if (packets == 0) return 0.0;
    return total_latency_.load() / packets;
}

size_t ClientSession::get_total_packets() const {
    return total_packets_.load();
}

size_t ClientSession::get_dropped_packets() const {
    return dropped_packets_.load();
}

void ClientSession::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    total_packets_ = 0;
    dropped_packets_ = 0;
    total_latency_ = 0;
    delay_compensation_ = 0;
    is_speaking_ = false;
    current_energy_ = 0;
}

// ============================================================================
// RoomAudioManager Implementation
// ============================================================================

RoomAudioManager::RoomAudioManager(const std::string& room_id)
    : room_id_(room_id)
    , last_alignment_time_(std::chrono::steady_clock::now())
    , rng_(std::random_device{}())
    , noise_dist_(-50, 50) {
    std::cout << "[RoomAudioManager] Created room: " << room_id_ << std::endl;
}

RoomAudioManager::~RoomAudioManager() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "[RoomAudioManager] Destroyed room: " << room_id_
              << " (Had " << clients_.size() << " clients, mixed " 
              << total_frames_mixed_.load() << " frames)" << std::endl;
}

void RoomAudioManager::add_client(const std::string& client_id, 
                                  std::shared_ptr<ClientSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_[client_id] = session;
    
    if (reference_client_.empty() && clients_.size() == 1) {
        reference_client_ = client_id;
        std::cout << "[RoomAudioManager] Room '" << room_id_ 
                  << "': Set reference client to " << client_id << std::endl;
    }
    
    std::cout << "[RoomAudioManager] Room '" << room_id_ 
              << "': Added client " << client_id
              << " (Total: " << clients_.size() << ")" << std::endl;
}

void RoomAudioManager::remove_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        clients_.erase(it);
        
        if (reference_client_ == client_id && !clients_.empty()) {
            reference_client_ = clients_.begin()->first;
            std::cout << "[RoomAudioManager] Room '" << room_id_
                      << "': New reference client: " << reference_client_ << std::endl;
        } else if (clients_.empty()) {
            reference_client_.clear();
        }
        
        std::cout << "[RoomAudioManager] Room '" << room_id_
                  << "': Removed client " << client_id
                  << " (Remaining: " << clients_.size() << ")" << std::endl;
    }
}

std::shared_ptr<ClientSession> RoomAudioManager::get_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(client_id);
    return (it != clients_.end()) ? it->second : nullptr;
}

bool RoomAudioManager::has_clients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !clients_.empty();
}

size_t RoomAudioManager::get_client_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
}

void RoomAudioManager::set_reference_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clients_.find(client_id) != clients_.end()) {
        reference_client_ = client_id;
        std::cout << "[RoomAudioManager] Room '" << room_id_
                  << "': Reference client changed to " << client_id << std::endl;
    }
}

std::vector<int16_t> RoomAudioManager::generate_comfort_noise(size_t num_samples) const {
    std::vector<int16_t> noise(num_samples);
    for (size_t i = 0; i < num_samples; ++i) {
        noise[i] = noise_dist_(rng_);
    }
    return noise;
}

std::vector<int16_t> RoomAudioManager::get_mixed_audio(size_t samples_needed) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    total_frames_mixed_++;
    
    if (clients_.empty()) {
        return generate_comfort_noise(samples_needed);
    }
    
    std::vector<std::vector<int16_t>> audio_streams;
    int active_speakers = 0;
    
    for (auto& pair : clients_) {
        auto& session = pair.second;
        if (session && session->has_audio()) {
            bool is_speaking = session->is_speaking();
            float energy = session->get_energy_level();
            
            // Use compensated frame (applies GCC-PHAT delay)
            auto frame = session->get_next_frame_compensated();
            if (!frame.empty()) {
                audio_streams.push_back(std::move(frame));
                active_speakers++;
            }
        }
    }
    
    if (audio_streams.empty()) {
        return generate_comfort_noise(samples_needed);
    }
    
    // Mix only active speakers with adaptive gain
    std::vector<int16_t> mixed = mix_audio_streams(audio_streams);
    
    // Log stats periodically
    if (total_frames_mixed_.load() % 200 == 0) {
        std::cout << "[RoomAudioManager] Room '" << room_id_
                  << "': Active speakers: " << active_speakers 
                  << "/" << clients_.size() << std::endl;
    }
    
    return mixed;
}
std::vector<int16_t> RoomAudioManager::mix_audio_streams(
    const std::vector<std::vector<int16_t>>& streams) const {
    
    if (streams.empty()) return {};
    
    size_t num_samples = streams[0].size();
    std::vector<int16_t> output(num_samples, 0);
    size_t num_streams = streams.size();
    
    // Adaptive gain: 1/sqrt(N) for speech, but limit maximum gain reduction
    float gain = 1.0f / std::sqrt(static_cast<float>(num_streams));
    gain = std::max(0.5f, gain);  // Don't reduce volume below 50%
    
    for (size_t i = 0; i < num_samples; ++i) {
        int32_t sum = 0;
        for (const auto& stream : streams) {
            if (i < stream.size()) {
                sum += stream[i];
            }
        }
        
        // Apply gain and soft clipping
        int32_t scaled = static_cast<int32_t>(static_cast<float>(sum) * gain);
        output[i] = soft_clip(scaled);
    }
    
    return output;
}

int16_t RoomAudioManager::soft_clip(int32_t sample) const {
    const float LIMIT = 32767.0f;
    float float_sample = static_cast<float>(sample);
    
    // Soft clipping for samples near the limit
    if (std::abs(float_sample) > LIMIT * 0.9f) {
        float over = std::abs(float_sample) - LIMIT * 0.9f;
        float sign = (float_sample > 0) ? 1.0f : -1.0f;
        float clipped = sign * (LIMIT * 0.9f + over * 0.1f);
        return static_cast<int16_t>(clipped);
    }
    
    if (float_sample > LIMIT) return 32767;
    if (float_sample < -LIMIT) return -32768;
    return static_cast<int16_t>(float_sample);
}

RoomStats RoomAudioManager::get_statistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    RoomStats stats;
    stats.total_clients = clients_.size();
    stats.total_packets_processed = total_frames_mixed_.load();
    
    double total_latency = 0.0;
    int latency_count = 0;
    
    for (const auto& pair : clients_) {
        auto& session = pair.second;
        if (session) {
            if (session->is_speaking()) {
                stats.active_speakers++;
            }
            if (session->get_total_packets() > 0) {
                total_latency += session->get_average_latency();
                latency_count++;
            }
            stats.total_dropped_packets += session->get_dropped_packets();
        }
    }
    
    if (latency_count > 0) {
        stats.average_latency_ms = total_latency / latency_count;
    }
    
    return stats;
}

} // namespace audio