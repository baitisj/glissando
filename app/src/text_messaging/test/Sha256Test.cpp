//=========================================================================
// Name:            Sha256Test.cpp
// Purpose:         SHA-256 against the standard test vectors (FIPS 180-4
//                  examples and NIST's), fed whole and in odd pieces.
//=========================================================================

#include <cstdio>
#include <string>
#include <vector>

#include "../Sha256.h"

using namespace TextMessaging;

namespace
{

int failures = 0;

void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        failures++;
        fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

std::string hashOf(const std::string& text)
{
    return Sha256::hex(Sha256::of((const uint8_t*)text.data(), text.size()));
}

// The same, fed a few bytes at a time.
std::string hashInPieces(const std::string& text, size_t step)
{
    Sha256 hash;
    for (size_t at = 0; at < text.size(); at += step)
    {
        size_t n = std::min(step, text.size() - at);
        hash.update((const uint8_t*)text.data() + at, n);
    }
    return Sha256::hex(hash.finish());
}

void testVectors()
{
    struct Vector
    {
        std::string input;
        const char* digest;
    };
    const Vector vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
        {"The quick brown fox jumps over the lazy dog",
         "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"},
    };
    for (const Vector& v : vectors)
    {
        CHECK(hashOf(v.input) == v.digest);
        for (size_t step : {(size_t)1, (size_t)3, (size_t)63, (size_t)64, (size_t)65})
        {
            CHECK(hashInPieces(v.input, step) == v.digest);
        }
    }

    // A million "a", the long FIPS example, in uneven pieces.
    std::string million(1000000, 'a');
    CHECK(hashInPieces(million, 4093) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Lengths at the padding's edges: 55, 56 and 64 bytes.
    CHECK(hashOf(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hashOf(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hashOf(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

void testFinishStartsAgain()
{
    Sha256 hash;
    hash.update((const uint8_t*)"abc", 3);
    Sha256::Digest first = hash.finish();
    hash.update((const uint8_t*)"abc", 3);
    CHECK(hash.finish() == first);
}

} // namespace

int main()
{
    testVectors();
    testFinishStartsAgain();

    if (failures > 0)
    {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all SHA-256 checks passed\n");
    return 0;
}
