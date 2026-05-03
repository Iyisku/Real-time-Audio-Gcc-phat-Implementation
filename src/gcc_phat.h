#ifndef GCC_PHAT_H
#define GCC_PHAT_H

#include <vector>
#include <complex>
#include <memory>
#include <kiss_fft.h>

namespace audio {

/**
 * GCC-PHAT (Generalized Cross-Correlation with Phase Transform)
 * Used to estimate time delay between two audio signals
 */
class GCCPHAT {
public:
    GCCPHAT();
    ~GCCPHAT();
    
    // Initialize with FFT size (must be power of 2)
    bool initialize(size_t fft_size);
    
    /**
     * Calculate time delay between reference and target signals
     * @param reference Reference audio samples (mono, int16_t)
     * @param target Target audio samples to compare against reference
     * @return Delay in samples. Positive = target lags behind reference.
     *         Negative = target leads reference.
     */
    int calculate_delay(const std::vector<int16_t>& reference, 
                        const std::vector<int16_t>& target);
    
    /**
     * Get last correlation peak value (confidence metric)
     * Higher value = more confident in delay estimate
     */
    float get_last_peak_confidence() const { return last_peak_confidence_; }
    
    /**
     * Get full cross-correlation result for debugging
     */
    std::vector<float> get_last_correlation() const { return last_correlation_; }
    
private:
    size_t fft_size_;
    size_t half_fft_size_;
    
    // FFT plans
    kiss_fft_cfg fwd_cfg_;
    kiss_fft_cfg inv_cfg_;
    
    // Buffers
    std::vector<kiss_fft_cpx> ref_fft_;
    std::vector<kiss_fft_cpx> target_fft_;
    std::vector<kiss_fft_cpx> cross_power_;
    
    // Results
    std::vector<float> correlation_;
    float last_peak_confidence_;
    std::vector<float> last_correlation_;
    
    // Window function (Hann window to reduce spectral leakage)
    std::vector<float> window_;
    
    void apply_window(std::vector<float>& signal);
    void compute_fft(const std::vector<float>& signal, std::vector<kiss_fft_cpx>& fft_out);
    void compute_ifft(const std::vector<kiss_fft_cpx>& freq_data, std::vector<float>& time_data);
    float find_peak_delay(const std::vector<float>& correlation, int& peak_index);
};

/**
 * DelayEstimator - Manages delay estimation for a room
 * Tracks multiple clients and maintains reference selection logic
 */
class DelayEstimator {
public:
    DelayEstimator();
    ~DelayEstimator();
    
    struct DelayResult {
        int delay_samples;      // Positive = client lags behind reference
        float confidence;       // 0-1 confidence in estimate
        bool valid;             // Whether estimate is reliable
    };
    
    /**
     * Initialize with FFT size and sample rate
     */
    void initialize(size_t fft_size, int sample_rate);
    
    /**
     * Update reference stream (usually the first active speaker or designated leader)
     */
    void set_reference(const std::vector<int16_t>& reference_samples);
    
    /**
     * Estimate delay for a client relative to current reference
     */
    DelayResult estimate_delay(const std::vector<int16_t>& client_samples);
    
    /**
     * Smart reference selection: Choose best stream as reference
     * @param clients Map of client_id -> audio samples
     */
    template<typename MapType>
    std::string select_best_reference(const MapType& client_samples) {
        if (client_samples.empty()) return "";
        
        // Select client with highest energy (likely the active speaker)
        std::string best_client;
        float max_energy = 0;
        
        for (const auto& [client_id, samples] : client_samples) {
            float energy = 0;
            for (auto s : samples) {
                energy += static_cast<float>(s) * s;
            }
            if (energy > max_energy) {
                max_energy = energy;
                best_client = client_id;
            }
        }
        
        // Only update if energy is significant (speaking, not silence)
        if (max_energy > 10000.0f) {
            set_reference(client_samples.at(best_client));
            return best_client;
        }
        
        return "";
    }
    
    void reset();
    
private:
    std::unique_ptr<GCCPHAT> gcc_;
    std::vector<int16_t> reference_buffer_;
    size_t fft_size_;
    int sample_rate_;
    float energy_threshold_;
};

} // namespace audio

#endif // GCC_PHAT_H