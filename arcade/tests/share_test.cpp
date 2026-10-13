// Share codes round-trip and ADPCM stays close to the sound it was given.
#include "../host/share.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

void hostLog(const char*, ...) {}
std::vector<uint8_t> httpGetBytes(const std::wstring&) { return {}; }

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { printf("FAIL: %s\n", what); fails++; }
    };
    // codes
    uint32_t ip = 0;
    uint16_t port = 0, secret = 0;
    std::wstring code = shareCodeText(0xC0A80105u, 6730, 0xABC);
    check(code.size() == 14 && code[4] == L'-' && code[9] == L'-', "code shape");
    check(shareCodeParse(code, ip, port, secret) && ip == 0xC0A80105u && port == 6730 && secret == 0xABC, "code round trip");
    std::wstring lower = code;
    for (auto& ch : lower) ch = wchar_t(towlower(ch));
    check(shareCodeParse(lower + L" ", ip, port, secret) && ip == 0xC0A80105u, "lower case and spaces accepted");
    check(!shareCodeParse(L"ABCD", ip, port, secret), "short code refused");
    check(!shareCodeParse(code + L"A", ip, port, secret), "long code refused");
    std::wstring pub = shareCodeText(0x08080808u, 65535, 0xfff);
    check(shareCodeParse(pub, ip, port, secret) && ip == 0x08080808u && port == 65535 && secret == 0xfff, "edge values");
    // ADPCM: a 440 Hz tone comes back within a few percent
    const int N = 4800;
    std::vector<int16_t> in(N), out(N);
    for (int i = 0; i < N; i++) in[size_t(i)] = int16_t(12000 * sin(i * 2 * 3.14159265 * 440 / 48000));
    std::vector<uint8_t> packed(N / 2);
    AdpcmState enc, dec;
    adpcmEncode(in.data(), N, enc, packed.data());
    adpcmDecode(packed.data(), N, dec, out.data());
    double err = 0, power = 0;
    for (int i = 200; i < N; i++) { double d = in[size_t(i)] - out[size_t(i)]; err += d * d; power += double(in[size_t(i)]) * in[size_t(i)]; }
    double ratio = sqrt(err / power);
    check(ratio < 0.05, "ADPCM error under 5 percent");
    check(enc.predictor == dec.predictor && enc.index == dec.index, "encoder and decoder states agree");
    if (fails) { printf("share_test: %d failure(s)\n", fails); return 1; }
    printf("share_test: all checks passed (ADPCM error %.1f%%)\n", ratio * 100);
    return 0;
}
