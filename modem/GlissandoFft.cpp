//=========================================================================
// Name:            GlissandoFft.cpp
// Purpose:         A small self-contained power of two FFT for the Glissando
//                  receiver (analytic signal and the sync search).
//=========================================================================

#include "GlissandoFft.h"

#include <cmath>

namespace
{
constexpr double PI = 3.14159265358979323846;
}

namespace Glissando
{

size_t nextPowerOfTwo(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

Fft::Fft(size_t size)
    : size_(size)
{
    for (size_t i = 0, j = 0; i < size_; i++)
    {
        if (i < j)
        {
            swaps_.push_back(i);
            swaps_.push_back(j);
        }
        size_t bit = size_ >> 1;
        while (bit != 0 && (j & bit))
        {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
    }

    twiddles_.resize(size_ > 1 ? size_ : 2);
    for (size_t k = 0; k < size_ / 2; k++)
    {
        double angle = 2.0 * PI * (double)k / (double)size_;
        twiddles_[2 * k] = std::cos(angle);
        twiddles_[2 * k + 1] = std::sin(angle);
    }
}

void Fft::forward(double* data) const
{
    transform(data, false);
}

void Fft::inverse(double* data) const
{
    transform(data, true);
}

void Fft::transform(double* data, bool inverse) const
{
    for (size_t i = 0; i < swaps_.size(); i += 2)
    {
        size_t a = 2 * swaps_[i];
        size_t b = 2 * swaps_[i + 1];
        double re = data[a];
        double im = data[a + 1];
        data[a] = data[b];
        data[a + 1] = data[b + 1];
        data[b] = re;
        data[b + 1] = im;
    }

    // The forward transform uses exp(-i angle), the inverse exp(+i angle).
    double sign = inverse ? 1.0 : -1.0;
    for (size_t half = 1; half < size_; half <<= 1)
    {
        size_t stride = size_ / (2 * half);
        for (size_t start = 0; start < size_; start += 2 * half)
        {
            double* a = data + 2 * start;
            double* b = data + 2 * (start + half);
            for (size_t k = 0; k < half; k++)
            {
                double wr = twiddles_[2 * k * stride];
                double wi = sign * twiddles_[2 * k * stride + 1];
                double tr = b[2 * k] * wr - b[2 * k + 1] * wi;
                double ti = b[2 * k] * wi + b[2 * k + 1] * wr;
                b[2 * k] = a[2 * k] - tr;
                b[2 * k + 1] = a[2 * k + 1] - ti;
                a[2 * k] += tr;
                a[2 * k + 1] += ti;
            }
        }
    }
}

} // namespace Glissando
