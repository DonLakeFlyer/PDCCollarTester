#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <airspy.h>

static constexpr double PI = 3.14159265358979323846;

// ── Configuration ──────────────────────────────────────────────────────────────

static constexpr uint32_t SAMPLE_RATE    = 3000000;   // 3 MSPS
static constexpr uint32_t SENSITIVITY    = 15;        // 0-21
static constexpr double   WARMUP_SECS    = 5.0;       // discard initial samples while AGC settles
static constexpr double   CAPTURE_SECS   = 10.0;      // must exceed max inter-pulse interval (2s)
static constexpr int      FFT_SIZE       = 4096;      // ~732 Hz per bin at 3 MSPS
static constexpr double   SIGNAL_BW_HZ   = 3000.0;    // only look ±3 kHz around center freq
static constexpr double   DEFAULT_MARGIN = 3.0;       // 3 dB = collar must be ≥50% of reference power
static constexpr double   MIN_SNR_DB     = 10.0;      // min peak-to-noise dB to consider a signal present
static constexpr int      REF_MAX_AGE_HOURS = 24;     // reference expires after this many hours

static const char* REF_FILENAME = "collar_ref.dat";

// ── Capture state ──────────────────────────────────────────────────────────────

struct CaptureContext {
    std::vector<float>         buffer;    // interleaved I,Q float32
    size_t                     target_iq_pairs;
    size_t                     collected; // count of floats (2 per IQ pair)
    std::mutex                 mtx;
    std::condition_variable    cv;
    bool                       done;
};

static int rx_callback(airspy_transfer_t* transfer) {
    auto* ctx = static_cast<CaptureContext*>(transfer->ctx);
    auto* samples = static_cast<const float*>(transfer->samples);
    size_t num_floats = static_cast<size_t>(transfer->sample_count) * 2;

    std::lock_guard<std::mutex> lock(ctx->mtx);
    size_t remaining = (ctx->target_iq_pairs * 2) - ctx->collected;
    size_t to_copy = std::min(num_floats, remaining);
    std::memcpy(ctx->buffer.data() + ctx->collected, samples, to_copy * sizeof(float));
    ctx->collected += to_copy;

    if (ctx->collected >= ctx->target_iq_pairs * 2) {
        ctx->done = true;
        ctx->cv.notify_one();
        return 1;
    }
    return 0;
}

// ── Simple DFT at DC (center freq) ────────────────────────────────────────────
// The collar frequency is the center frequency, so its energy is at DC (bin 0)
// in baseband. We compute a Welch-style averaged periodogram: chop the capture
// into overlapping Hann-windowed segments, FFT each, average the magnitude².
// Then compare the peak bin to the median (noise floor).
//
// We implement a radix-2 Cooley-Tukey FFT to avoid external dependencies.

static void fft_inplace(std::vector<double>& re, std::vector<double>& im, int n) {
    // Bit-reversal permutation
    int log2n = 0;
    for (int tmp = n; tmp > 1; tmp >>= 1) log2n++;

    for (int i = 0; i < n; i++) {
        int j = 0;
        for (int b = 0; b < log2n; b++)
            if (i & (1 << b)) j |= (1 << (log2n - 1 - b));
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    // Cooley-Tukey butterflies
    for (int size = 2; size <= n; size *= 2) {
        int half = size / 2;
        double angle = -2.0 * PI / size;
        double wRe = std::cos(angle);
        double wIm = std::sin(angle);
        for (int start = 0; start < n; start += size) {
            double curRe = 1.0, curIm = 0.0;
            for (int k = 0; k < half; k++) {
                int even = start + k;
                int odd  = start + k + half;
                double tRe = curRe * re[odd] - curIm * im[odd];
                double tIm = curRe * im[odd] + curIm * re[odd];
                re[odd] = re[even] - tRe;
                im[odd] = im[even] - tIm;
                re[even] += tRe;
                im[even] += tIm;
                double newCurRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = newCurRe;
            }
        }
    }
}

struct PowerResult {
    double peak_db;      // peak PSD bin in dBFS
    double noise_db;     // median PSD bin in dBFS (noise floor)
    double snr_db;       // peak - noise
};

static PowerResult measure_power(const std::vector<float>& buf, size_t num_iq_pairs) {
    PowerResult result = { -999.0, -999.0, 0.0 };

    if (num_iq_pairs < static_cast<size_t>(FFT_SIZE))
        return result;

    // Precompute Hann window
    std::vector<double> hann(FFT_SIZE);
    double hann_norm = 0.0;
    for (int i = 0; i < FFT_SIZE; i++) {
        hann[i] = 0.5 * (1.0 - std::cos(2.0 * PI * i / FFT_SIZE));
        hann_norm += hann[i] * hann[i];
    }

    // Welch: 50% overlap segments
    int step = FFT_SIZE / 2;
    int num_segments = 0;
    std::vector<double> avg_psd(FFT_SIZE, 0.0);
    std::vector<double> re(FFT_SIZE), im(FFT_SIZE);

    for (size_t offset = 0; offset + FFT_SIZE <= num_iq_pairs; offset += step) {
        std::fill(re.begin(), re.end(), 0.0);
        std::fill(im.begin(), im.end(), 0.0);
        for (int i = 0; i < FFT_SIZE; i++) {
            size_t idx = offset + i;
            re[i] = buf[2 * idx]     * hann[i];
            im[i] = buf[2 * idx + 1] * hann[i];
        }

        fft_inplace(re, im, FFT_SIZE);

        for (int i = 0; i < FFT_SIZE; i++) {
            avg_psd[i] += (re[i] * re[i] + im[i] * im[i]);
        }
        num_segments++;
    }

    if (num_segments == 0)
        return result;

    // Average and normalize
    for (int i = 0; i < FFT_SIZE; i++) {
        avg_psd[i] /= (num_segments * hann_norm);
    }

    // Bin 0 = DC = center frequency. FFT bins are ordered:
    //   [0, 1, ..., N/2-1, -N/2, ..., -1]
    // Bins near DC correspond to frequencies near the center freq.
    // signal_bins = number of bins within ±SIGNAL_BW_HZ of center
    double bin_hz = static_cast<double>(SAMPLE_RATE) / FFT_SIZE;
    int signal_bins = static_cast<int>(std::ceil(SIGNAL_BW_HZ / bin_hz));

    // Peak power: max of bins within ±signal_bins of DC
    double peak = 0.0;
    for (int i = 0; i <= signal_bins; i++) {
        if (avg_psd[i] > peak) peak = avg_psd[i];                          // positive freq side
        int neg = FFT_SIZE - i;
        if (neg < FFT_SIZE && avg_psd[neg] > peak) peak = avg_psd[neg];    // negative freq side
    }

    // Noise floor: median of bins OUTSIDE the signal region
    std::vector<double> noise_bins;
    noise_bins.reserve(FFT_SIZE);
    for (int i = signal_bins + 1; i < FFT_SIZE - signal_bins; i++) {
        noise_bins.push_back(avg_psd[i]);
    }
    std::sort(noise_bins.begin(), noise_bins.end());
    double median = noise_bins[noise_bins.size() / 2];

    result.peak_db  = 10.0 * std::log10(peak + 1e-30);
    result.noise_db = 10.0 * std::log10(median + 1e-30);
    result.snr_db   = result.peak_db - result.noise_db;
    return result;
}

// ── Reference file I/O ────────────────────────────────────────────────────────

static std::filesystem::path ref_file_path(const char* argv0) {
    std::error_code ec;
    auto exe_path = std::filesystem::weakly_canonical(argv0, ec);
    if (ec || exe_path.empty())
        return std::filesystem::current_path() / REF_FILENAME;
    return exe_path.parent_path() / REF_FILENAME;
}

static bool save_reference(const std::filesystem::path& path, uint32_t freq_hz, double power_db) {
    std::ofstream f(path);
    if (!f) return false;
    f << freq_hz << " " << power_db << "\n";
    return f.good();
}

static bool load_reference(const std::filesystem::path& path, uint32_t& freq_hz, double& power_db) {
    std::ifstream f(path);
    if (!f) return false;
    return static_cast<bool>(f >> freq_hz >> power_db);
}

// ── Airspy helpers ─────────────────────────────────────────────────────────────

static void check(int result, const char* what) {
    if (result != AIRSPY_SUCCESS) {
        fprintf(stderr, "Error: %s failed: %s\n", what, airspy_error_name(static_cast<airspy_error>(result)));
        exit(1);
    }
}

static PowerResult capture_power(uint32_t freq_hz) {
    struct airspy_device* device = nullptr;

    check(airspy_open(&device), "airspy_open");
    check(airspy_set_sample_type(device, AIRSPY_SAMPLE_FLOAT32_IQ), "set_sample_type");
    check(airspy_set_samplerate(device, SAMPLE_RATE), "set_samplerate");
    check(airspy_set_freq(device, freq_hz), "set_freq");
    check(airspy_set_sensitivity_gain(device, SENSITIVITY), "set_sensitivity_gain");

    // Warmup: capture and discard initial samples while AGC/PLL settle
    size_t warmup_iq_pairs = static_cast<size_t>(SAMPLE_RATE * WARMUP_SECS);

    CaptureContext warmup_ctx;
    warmup_ctx.buffer.resize(warmup_iq_pairs * 2);
    warmup_ctx.target_iq_pairs = warmup_iq_pairs;
    warmup_ctx.collected = 0;
    warmup_ctx.done = false;

    check(airspy_start_rx(device, rx_callback, &warmup_ctx), "start_rx (warmup)");

    {
        std::unique_lock<std::mutex> lock(warmup_ctx.mtx);
        auto timeout = std::chrono::seconds(static_cast<int>(WARMUP_SECS) + 5);
        if (!warmup_ctx.cv.wait_for(lock, timeout, [&] { return warmup_ctx.done; })) {
            airspy_stop_rx(device);
            airspy_close(device);
            fprintf(stderr, "Error: warmup timed out. Check Airspy connection.\n");
            exit(1);
        }
    }

    airspy_stop_rx(device);

    // Now capture the real data
    size_t total_iq_pairs = static_cast<size_t>(SAMPLE_RATE * CAPTURE_SECS);

    CaptureContext ctx;
    ctx.buffer.resize(total_iq_pairs * 2);
    ctx.target_iq_pairs = total_iq_pairs;
    ctx.collected = 0;
    ctx.done = false;

    check(airspy_start_rx(device, rx_callback, &ctx), "start_rx");

    {
        std::unique_lock<std::mutex> lock(ctx.mtx);
        auto timeout = std::chrono::seconds(static_cast<int>(CAPTURE_SECS) + 5);
        if (!ctx.cv.wait_for(lock, timeout, [&] { return ctx.done; })) {
            airspy_stop_rx(device);
            airspy_close(device);
            fprintf(stderr, "Error: capture timed out. Check Airspy connection.\n");
            exit(1);
        }
    }

    airspy_stop_rx(device);
    airspy_close(device);

    size_t captured_pairs = ctx.collected / 2;
    return measure_power(ctx.buffer, captured_pairs);
}

// ── Usage ──────────────────────────────────────────────────────────────────────

static void usage(const char* prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s --freq <MHz> --calibrate     Calibrate with a known-good collar\n", prog);
    fprintf(stderr, "  %s --freq <MHz>                  Test a collar against reference\n", prog);
    fprintf(stderr, "  %s --freq <MHz> --margin <dB>    Test with custom margin (default: %.1f dB)\n", prog, DEFAULT_MARGIN);
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s --freq 148.500 --calibrate\n", prog);
    fprintf(stderr, "  %s --freq 148.500\n", prog);
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    double freq_mhz = 0.0;
    double margin_db = DEFAULT_MARGIN;
    bool calibrate = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--freq") == 0 && i + 1 < argc) {
            freq_mhz = atof(argv[++i]);
        } else if (strcmp(argv[i], "--margin") == 0 && i + 1 < argc) {
            margin_db = atof(argv[++i]);
        } else if (strcmp(argv[i], "--calibrate") == 0) {
            calibrate = true;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (freq_mhz <= 0.0) {
        usage(argv[0]);
        return 1;
    }

    uint32_t freq_hz = static_cast<uint32_t>(std::round(freq_mhz * 1e6));

    // In test mode, verify reference file exists before capturing
    std::filesystem::path ref_path;
    uint32_t ref_freq_hz = 0;
    double ref_power_db = 0.0;
    if (!calibrate) {
        ref_path = ref_file_path(argv[0]);
        if (!load_reference(ref_path, ref_freq_hz, ref_power_db)) {
            fprintf(stderr, "Error: no reference file found at %s\n", ref_path.c_str());
            fprintf(stderr, "Run with --calibrate first using a known-good collar.\n");
            return 1;
        }
        // Check reference file age
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(ref_path, ec);
        if (!ec) {
            auto age = std::filesystem::file_time_type::clock::now() - ftime;
            auto age_hours = std::chrono::duration_cast<std::chrono::hours>(age).count();
            if (age_hours >= REF_MAX_AGE_HOURS) {
                fprintf(stderr, "Error: reference calibration is %ld hours old (max %d hours).\n",
                        static_cast<long>(age_hours), REF_MAX_AGE_HOURS);
                fprintf(stderr, "Run with --calibrate to re-calibrate.\n");
                return 1;
            }
        }
    }

    printf("Capturing at %.3f MHz for %.1f seconds...\n", freq_mhz, CAPTURE_SECS);
    PowerResult pr = capture_power(freq_hz);
    printf("Peak: %.1f dB | Noise floor: %.1f dB | SNR: %.1f dB\n",
           pr.peak_db, pr.noise_db, pr.snr_db);

    if (pr.snr_db < MIN_SNR_DB) {
        printf("\nNo signal detected (SNR %.1f dB < %.1f dB minimum)\n", pr.snr_db, MIN_SNR_DB);
        if (calibrate) {
            fprintf(stderr, "Error: cannot calibrate without a collar signal. Turn the collar on.\n");
            return 1;
        }
        printf("\n  *** BAD ***  (no signal)\n\n");
        return 1;
    }

    if (calibrate) {
        ref_path = ref_file_path(argv[0]);
        if (save_reference(ref_path, freq_hz, pr.peak_db)) {
            printf("Reference saved to %s\n", ref_path.c_str());
            printf("Reference power: %.1f dBFS at %.3f MHz\n", pr.peak_db, freq_mhz);
        } else {
            fprintf(stderr, "Error: failed to save reference file\n");
            return 1;
        }
        return 0;
    }

    // Test mode — compare against reference
    double diff = ref_power_db - pr.peak_db;

    printf("Reference: %.1f dBFS | Measured: %.1f dBFS | Diff: %.1f dB | Margin: %.1f dB\n",
           ref_power_db, pr.peak_db, diff, margin_db);

    double pct = 100.0 * std::pow(10.0, -diff / 10.0);
    printf("Signal strength: %.0f%% of reference\n", pct);

    if (diff <= margin_db) {
        printf("\n  *** GOOD ***\n\n");
        return 0;
    } else {
        printf("\n  *** BAD ***\n\n");
        return 1;
    }
}
