#include "gcc_phat.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <cstring>

namespace audio {

// ============================================================================
// GCCPHAT Implementation
// ============================================================================

GCCPHAT::GCCPHAT() 
    : fft_size_(0)
    , half_fft_size_(0)
    , fwd_cfg_(nullptr)
    , inv_cfg_(nullptr)
    , last_peak_confidence_(0.0f) {}

GCCPHAT::~GCCPHAT() {
    if (fwd_cfg_) {
        kiss_fft_free(fwd_cfg_);
    }
    if (inv_cfg_) {
        kiss_fft_free(inv_cfg_);
    }
}

bool GCCPHAT::initialize(size_t fft_size) {
    // Verify power of 2
    if ((fft_size & (fft_size - 1)) != 0) {
        std::cerr << "[GCCPHAT] FFT size must be power of 2. Got: " << fft_size << std::endl;
        return false;
    }
    
    fft_size_ = fft_size;
    half_fft_size_ = fft_size / 2;
    
    // Create FFT plans
    fwd_cfg_ = kiss_fft_alloc(fft_size_, 0, nullptr, nullptr);
    inv_cfg_ = kiss_fft_alloc(fft_size_, 1, nullptr, nullptr);
    
    if (!fwd_cfg_ || !inv_cfg_) {
        std::cerr << "[GCCPHAT] Failed to create FFT plans" << std::endl;
        return false;
    }
    
    // Allocate buffers
    ref_fft_.resize(fft_size_);
    target_fft_.resize(fft_size_);
    cross_power_.resize(fft_size_);
    correlation_.resize(fft_size_);
    
    // Create Hann window
    window_.resize(fft_size_);
    for (size_t i = 0; i < fft_size_; ++i) {
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (fft_size_ - 1)));
    }
    
    std::cout << "[GCCPHAT] Initialized with FFT size: " << fft_size_ << std::endl;
    return true;
}

void GCCPHAT::apply_window(std::vector<float>& signal) {
    for (size_t i = 0; i < fft_size_ && i < signal.size(); ++i) {
        signal[i] *= window_[i];
    }
}

void GCCPHAT::compute_fft(const std::vector<float>& signal, 
                          std::vector<kiss_fft_cpx>& fft_out) {
    // Prepare input (real to complex)
    std::vector<kiss_fft_cpx> input(fft_size_);
    for (size_t i = 0; i < fft_size_; ++i) {
        float val = (i < signal.size()) ? signal[i] : 0.0f;
        input[i].r = val;
        input[i].i = 0.0f;
    }
    
    // Execute FFT
    kiss_fft(fwd_cfg_, input.data(), fft_out.data());
}

void GCCPHAT::compute_ifft(const std::vector<kiss_fft_cpx>& freq_data,
                           std::vector<float>& time_data) {
    std::vector<kiss_fft_cpx> output(fft_size_);
    kiss_fft(inv_cfg_, freq_data.data(), output.data());
    
    // Convert complex to real (take real part and normalize)
    time_data.resize(fft_size_);
    for (size_t i = 0; i < fft_size_; ++i) {
        time_data[i] = output[i].r / fft_size_;
    }
}

int GCCPHAT::calculate_delay(const std::vector<int16_t>& reference,
                             const std::vector<int16_t>& target) {
    
    if (fft_size_ == 0) {
        std::cerr << "[GCCPHAT] Not initialized. Call initialize() first." << std::endl;
        return 0;
    }
    
    // Convert int16 to float and ensure proper length
    std::vector<float> ref_float(fft_size_, 0.0f);
    std::vector<float> target_float(fft_size_, 0.0f);
    
    size_t copy_size = std::min(fft_size_, reference.size());
    for (size_t i = 0; i < copy_size; ++i) {
        ref_float[i] = static_cast<float>(reference[i]) / 32768.0f;
    }
    
    copy_size = std::min(fft_size_, target.size());
    for (size_t i = 0; i < copy_size; ++i) {
        target_float[i] = static_cast<float>(target[i]) / 32768.0f;
    }
    
    // Apply window to reduce spectral leakage
    apply_window(ref_float);
    apply_window(target_float);
    
    // Compute FFT of both signals
    compute_fft(ref_float, ref_fft_);
    compute_fft(target_float, target_fft_);
    
    // GCC-PHAT: Cross-power spectrum with phase transform
    // R = FFT(ref) * conj(FFT(target))
    // R_phat = R / |R| (ignore magnitude, keep phase only)
    
    const float EPSILON = 1e-10f;
    
    for (size_t i = 0; i < fft_size_; ++i) {
        // Complex multiplication: ref * conj(target)
        float real = ref_fft_[i].r * target_fft_[i].r + ref_fft_[i].i * target_fft_[i].i;
        float imag = ref_fft_[i].i * target_fft_[i].r - ref_fft_[i].r * target_fft_[i].i;
        
        // Magnitude
        float mag = std::sqrt(real * real + imag * imag);
        
        // PHAT weighting: normalize to unit magnitude (preserve phase only)
        if (mag > EPSILON) {
            cross_power_[i].r = real / mag;
            cross_power_[i].i = imag / mag;
        } else {
            cross_power_[i].r = 0;
            cross_power_[i].i = 0;
        }
    }
    
    // Inverse FFT to get cross-correlation
    compute_ifft(cross_power_, correlation_);
    
    // Store for debugging
    last_correlation_ = correlation_;
    
    // Find peak (delay estimate)
    int peak_index = 0;
    last_peak_confidence_ = find_peak_delay(correlation_, peak_index);
    
    // Convert peak index to delay in samples
    // If peak is in second half of array, it represents negative delay
    int delay = peak_index;
    if (peak_index > static_cast<int>(half_fft_size_)) {
        delay = peak_index - fft_size_;
    }
    
    return delay;
}

float GCCPHAT::find_peak_delay(const std::vector<float>& correlation, int& peak_index) {
    peak_index = 0;
    float max_val = 0.0f;
    float second_max = 0.0f;
    
    // Find peak (ignoring the zero-lag region for stability)
    int start_idx = 5;  // Skip small lags to avoid noise
    int end_idx = correlation.size() - 5;
    
    for (size_t i = start_idx; i < static_cast<size_t>(end_idx); ++i) {
        float abs_val = std::abs(correlation[i]);
        if (abs_val > max_val) {
            second_max = max_val;
            max_val = abs_val;
            peak_index = i;
        } else if (abs_val > second_max) {
            second_max = abs_val;
        }
    }
    
    // Confidence = peak / (second_peak + epsilon)
    float confidence = max_val / (second_max + 0.1f);
    
    // Also check that peak is reasonably sharp
    if (peak_index > 0 && peak_index < static_cast<int>(correlation.size()) - 1) {
        float left = std::abs(correlation[peak_index - 1]);
        float right = std::abs(correlation[peak_index + 1]);
        float sharpness = max_val / ((left + right) / 2.0f + 0.1f);
        confidence = std::min(confidence, sharpness);
    }
    
    return std::min(1.0f, confidence / 5.0f);  // Normalize to 0-1
}

// ============================================================================
// DelayEstimator Implementation
// ============================================================================

DelayEstimator::DelayEstimator() 
    : fft_size_(0)
    , sample_rate_(48000)
    , energy_threshold_(10000.0f) {}

DelayEstimator::~DelayEstimator() = default;

void DelayEstimator::initialize(size_t fft_size, int sample_rate) {
    fft_size_ = fft_size;
    sample_rate_ = sample_rate;
    gcc_ = std::make_unique<GCCPHAT>();
    gcc_->initialize(fft_size);
    
    std::cout << "[DelayEstimator] Initialized: FFT=" << fft_size 
              << ", SR=" << sample_rate << std::endl;
}

void DelayEstimator::set_reference(const std::vector<int16_t>& reference_samples) {
    reference_buffer_ = reference_samples;
    
    // Log energy for debugging
    float energy = 0;
    for (auto s : reference_buffer_) {
        energy += static_cast<float>(s) * s;
    }
    energy /= reference_buffer_.size();
    
    std::cout << "[DelayEstimator] Reference updated. Energy: " << energy << std::endl;
}

DelayEstimator::DelayResult DelayEstimator::estimate_delay(
    const std::vector<int16_t>& client_samples) {
    
    DelayResult result;
    result.valid = false;
    result.delay_samples = 0;
    result.confidence = 0.0f;
    
    if (!gcc_ || reference_buffer_.empty() || client_samples.empty()) {
        return result;
    }
    
    // Estimate delay using GCC-PHAT
    int delay = gcc_->calculate_delay(reference_buffer_, client_samples);
    float confidence = gcc_->get_last_peak_confidence();
    
    // Validate result
    int max_reasonable_delay = 100;  // ~2ms at 48kHz (reasonable for network)
    if (confidence > 0.3f && std::abs(delay) < max_reasonable_delay) {
        result.valid = true;
        result.delay_samples = delay;
        result.confidence = confidence;
        
        std::cout << "[DelayEstimator] Delay: " << delay << " samples (" 
                  << (delay * 1000.0f / sample_rate_) << " ms), Confidence: " 
                  << confidence << std::endl;
    } else if (confidence > 0.1f) {
        // Low confidence but still plausible
        result.valid = true;
        result.delay_samples = delay;
        result.confidence = confidence;
        
        std::cout << "[DelayEstimator] Low confidence delay: " << delay 
                  << " samples, Conf: " << confidence << std::endl;
    }
    
    return result;
}

void DelayEstimator::reset() {
    reference_buffer_.clear();
    if (gcc_) {
        // Reinitialize to clear state
        gcc_ = std::make_unique<GCCPHAT>();
        gcc_->initialize(fft_size_);
    }
}

} // namespace audio