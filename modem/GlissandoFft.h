//=========================================================================
// Name:            GlissandoFft.h
// Purpose:         A small self-contained power of two FFT for the Glissando
//                  receiver (analytic signal and the sync search).
//=========================================================================

#ifndef GLISSANDO__GLISSANDO_FFT_H
#define GLISSANDO__GLISSANDO_FFT_H

#include <cstddef>
#include <vector>

namespace Glissando
{

// Smallest power of two >= n (and >= 1).
size_t nextPowerOfTwo(size_t n);

// Radix-2 decimation in time, in place, on interleaved (re, im) doubles.
// An Fft object is immutable after construction, so one can be shared by
// threads.
class Fft
{
public:
    explicit Fft(size_t size); // size must be a power of two

    size_t size() const { return size_; }

    // X[k] = sum x[n] exp(-2 pi i k n / N)
    void forward(double* data) const;

    // x[n] = sum X[k] exp(+2 pi i k n / N), not divided by N.
    void inverse(double* data) const;

private:
    void transform(double* data, bool inverse) const;

    size_t size_;
    std::vector<size_t> swaps_;     // bit reversal permutation, as (i, j) pairs
    std::vector<double> twiddles_;  // cos, sin of 2 pi k / N for k < N / 2
};

} // namespace Glissando

#endif // GLISSANDO__GLISSANDO_FFT_H
