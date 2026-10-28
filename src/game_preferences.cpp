#include "launcher.h"
#include "cpu/x86_context.h"
#include "kernel/kernel.h"
#include "game_preferences.h"
#include "netplay.h"
#include <bit>

void openshao::preferences::Apply(X86Context& ctx, uint8_t*) {
    static bool applied = false;
    const auto& settings = launcher::Current();
    if (applied || (!settings.original_override && !netplay::IsActive())) return;
    applied = true;
    ApplyNow(ctx,settings);
}
void openshao::preferences::ApplyNow(X86Context& ctx,const launcher::Settings& settings) {
    const auto saved = ctx;
    const int volumes[] = {settings.music_volume, settings.effects_volume, settings.speech_volume};
    for (unsigned i = 0; i < 3; ++i) {
        const auto value = std::bit_cast<uint32_t>(volumes[i] / 100.f);
        kernel::W32(0x4DF5C4 + 4 * i, value);
        const auto category = kernel::CallGuest(ctx, 0x150E30, {i});
        kernel::CallGuest(ctx, 0x150DB0, {category, value});
    }
    const auto brightness = std::bit_cast<uint32_t>(settings.brightness / 100.f);
    kernel::W32(0x4DF5C0, brightness);
    kernel::CallGuest(ctx, 0x123A50, {brightness});
    // In Netplay, standardize gameplay-affecting match rules so Host and Client cannot diverge.
    const bool net = netplay::IsActive();
    const int rounds = net ? 3 : settings.versus_rounds;
    const int match_time = net ? 120 : settings.versus_time;
    const bool cam_rev = net ? false : settings.camera_reverse;
    kernel::G<uint8_t>(0x4DF5D1) = rounds == 1;
    kernel::G<uint8_t>(0x4DF5D2) = match_time == 120 ? 0 : match_time == 180 ? 1 : 2;
    kernel::G<uint8_t>(0x4DF5D3) = cam_rev;
    kernel::G<uint8_t>(0x599848) = cam_rev;
    ctx = saved;
    LOGR("Original options applied: music=%d effects=%d speech=%d brightness=%d rounds=%d time=%d camera_reverse=%d",
         settings.music_volume, settings.effects_volume, settings.speech_volume, settings.brightness,
         rounds, match_time, int(cam_rev));
}
void openshao::preferences::ReadActive(launcher::Settings& s) {
    auto volume=[](uint32_t address){return std::clamp(int(std::lround(std::bit_cast<float>(kernel::R32(address))*100)),0,100);};
    s.music_volume=volume(0x4DF5C4);s.effects_volume=volume(0x4DF5C8);s.speech_volume=volume(0x4DF5CC);s.brightness=volume(0x4DF5C0);
    s.versus_rounds=kernel::G<uint8_t>(0x4DF5D1)?1:3;
    const auto time=kernel::G<uint8_t>(0x4DF5D2);s.versus_time=time==0?120:time==1?180:0;
    s.camera_reverse=kernel::G<uint8_t>(0x4DF5D3)!=0;
}
