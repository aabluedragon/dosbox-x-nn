/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu.h"
#include "pic.h"
#include "vga.h"
#include <gtest/gtest.h>

Bitu vga_read_p3da(Bitu port, Bitu iolen);
extern bool vga_render_on_demand;
extern bool enable_vretrace_poll_debugging_marker;
extern unsigned char vga_p3da_undefined_bits;

namespace {

class VGAStatusTest : public ::testing::Test {
    decltype(vga.draw.delay) old_delay = vga.draw.delay;
    MachineType old_machine = machine;
    SVGACards old_card = svgaCard;
    cpu_cycles_count_t old_max = CPU_CycleMax, old_left = CPU_CycleLeft;
    cpu_cycles_count_t old_cycles = CPU_Cycles;
    Bitu old_ticks = PIC_Ticks;
    bool old_demand = vga_render_on_demand;
    bool old_marker = enable_vretrace_poll_debugging_marker;
    bool old_lockout = vga.dosboxig.vga_3da_lockout;
    bool old_attr_index = vga.internal.attrindex;
    bool old_pcjr_flipflop = vga.tandy.pcjr_flipflop;
    unsigned char old_undefined = vga_p3da_undefined_bits;
protected:
    void SetUp() override {
        machine = MCH_VGA;
        svgaCard = SVGA_None;
        CPU_CycleMax = CPU_CycleLeft = 1000;
        CPU_Cycles = 0;
        PIC_Ticks = 0;
        vga_render_on_demand = false;
        enable_vretrace_poll_debugging_marker = false;
        vga.dosboxig.vga_3da_lockout = false;
        vga_p3da_undefined_bits = 0;
        vga.draw.delay.htotal = 10;
        vga.draw.delay.hdend = 7;
        vga.draw.delay.hblkstart = 8;
        vga.draw.delay.hblkend = 9;
        vga.draw.delay.vdend = 80;
        vga.draw.delay.vblkstart = 84;
        vga.draw.delay.vblkend = 98;
        vga.draw.delay.vrstart = 90;
        vga.draw.delay.vrend = 92;
        vga.draw.delay.vtotal = 100;
    }
    void TearDown() override {
        vga.draw.delay = old_delay;
        machine = old_machine;
        svgaCard = old_card;
        CPU_CycleMax = old_max;
        CPU_CycleLeft = old_left;
        CPU_Cycles = old_cycles;
        PIC_Ticks = old_ticks;
        vga_render_on_demand = old_demand;
        enable_vretrace_poll_debugging_marker = old_marker;
        vga.dosboxig.vga_3da_lockout = old_lockout;
        vga.internal.attrindex = old_attr_index;
        vga.tandy.pcjr_flipflop = old_pcjr_flipflop;
        vga_p3da_undefined_bits = old_undefined;
    }
    Bitu At(double time, Bitu port = 0x3da) {
        // PIC_FullIndex is zero, so this places the beam precisely in the frame.
        vga.draw.delay.framestart = -time;
        return vga_read_p3da(port, 1);
    }
};

TEST_F(VGAStatusTest, DisplayEnableEndsBeforeBlankingPulse) {
    EXPECT_EQ(0u, At(6.999) & 1);
    EXPECT_EQ(1u, At(7.0) & 1);
    EXPECT_EQ(1u, At(7.5) & 1);
}

TEST_F(VGAStatusTest, BackPorchDoesNotBecomeActiveDisplay) {
    for (const auto type : {MCH_EGA, MCH_VGA}) {
        machine = type;
        EXPECT_EQ(1u, At(8.5) & 1);  // blanking pulse
        EXPECT_EQ(1u, At(9.5) & 1);  // back porch, after HBlank End
        EXPECT_EQ(1u, At(9.999) & 1);
        EXPECT_EQ(0u, At(10.0) & 1); // next line's active display
    }
}

TEST_F(VGAStatusTest, ActiveDisplayRepeatsEachScanline) {
    for (unsigned i = 0; i < 800; i++) {
        const double time = i / 10.0;
        EXPECT_EQ((i % 100 >= 70) ? 1u : 0u, At(time) & 1) << time;
    }
}

TEST_F(VGAStatusTest, VerticalDisplayEndAndRetraceAreSeparateSignals) {
    EXPECT_EQ(0u, At(76.5) & 9);
    EXPECT_EQ(1u, At(80.0) & 9);
    EXPECT_EQ(1u, At(81.0) & 9);
    EXPECT_EQ(9u, At(90.5) & 9);
    EXPECT_EQ(1u, At(92.5) & 9);
}

TEST_F(VGAStatusTest, StatusReadStillResetsAttributeFlipflop) {
    vga.internal.attrindex = true;
    vga.tandy.pcjr_flipflop = true;
    EXPECT_EQ(0u, At(1.0, 0x3ba) & 1);
    EXPECT_FALSE(vga.internal.attrindex);
    EXPECT_FALSE(vga.tandy.pcjr_flipflop);
}

TEST_F(VGAStatusTest, ConfiguredUndefinedBitsArePreserved) {
    vga_p3da_undefined_bits = 0x30;
    EXPECT_EQ(0x30u, At(1.0));
    EXPECT_EQ(0x31u, At(9.5));
    EXPECT_EQ(0x39u, At(90.5));
}

TEST_F(VGAStatusTest, TsengRetraceComplementRemainsCorrect) {
    svgaCard = SVGA_TsengET4K;
    EXPECT_EQ(0x81u, At(81.0));
    EXPECT_EQ(0x09u, At(90.5));
}

TEST_F(VGAStatusTest, IntegrationDeviceStatusLockoutRemainsEffective) {
    vga.dosboxig.vga_3da_lockout = true;
    EXPECT_EQ(0u, At(90.5));
}

} // namespace
