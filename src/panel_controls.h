// Controls on the panel art. x and y are in a 2576 x 687 frame of the cropped manual page.
// channel and mux from the Panel A and B potentiometer sheets, bar and bit from the button sheets.
// Positions come from the vector art (rom/panel.ob12p): knob = midpoint of its two gradient stacks,
// slider = centre of its 8 x 88 track, key = centre of its ellipse. The cursor pad is kept by hand.
// Formerly read off the image by eye. Where one sheet label covers several controls, as with
// the three Wave Mix sliders and Wave Select buttons, the left to right order is assumed.
#pragma once

struct PanelPot { const char* name; float x, y; bool slider; int channel, mux; };
struct PanelKey { const char* name; float x, y; int bar, bit; };

static const float panel_frame_w = 2576, panel_frame_h = 687;

static const PanelPot panel_pots[] = {
    {"LFO1 Rate", 118, 73, false, 1, 7}, {"LFO1 Fade", 117, 162, false, 1, 5},
    {"Osc2 Level", 260, 103, true, 1, 6}, {"Ring Mod", 313, 103, true, 1, 4}, {"Noise", 367, 103, true, 1, 2},
    {"Common Attack", 448, 103, true, 2, 5}, {"Common Decay", 501, 103, true, 2, 7}, {"Common Env Depth", 585, 162, false, 2, 6},
    {"Osc1 Mix 1", 702, 102, true, 2, 4}, {"Osc1 Mix 2", 755, 102, true, 3, 0}, {"Osc1 Mix 3", 808, 102, true, 3, 3},
    {"Osc1 FM", 893, 74, false, 3, 7}, {"Osc1 LFO1 Depth", 973, 74, false, 3, 6},
    {"Osc1 Wave Ctrl", 891, 162, false, 3, 5}, {"Osc1 PWM Depth", 973, 162, false, 3, 4},
    {"Osc2 Mix 1", 1210, 102, true, 4, 7}, {"Osc2 Mix 2", 1263, 102, true, 4, 5}, {"Osc2 Mix 3", 1316, 102, true, 4, 3},
    {"Osc2 LFO1 Depth", 1480, 74, false, 4, 6}, {"Osc2 Fine", 1560, 74, false, 4, 4},
    {"Osc2 Width", 1399, 162, false, 4, 0}, {"Osc2 PWM Depth", 1479, 162, false, 4, 1}, {"Osc2 Range", 1561, 162, false, 4, 2},
    {"Filter Frequency", 1695, 75, false, 5, 5}, {"Filter KBD Track", 1785, 75, false, 5, 7}, {"Filter Resonance", 1696, 162, false, 5, 3},
    {"Filter Attack", 1878, 102, true, 5, 6}, {"Filter Decay", 1931, 102, true, 5, 4},
    {"Filter Sustain", 1984, 102, true, 6, 5}, {"Filter Release", 2039, 102, true, 6, 7},
    {"Filter Env Depth", 2135, 75, false, 6, 6}, {"Filter LFO1 Depth", 2135, 162, false, 6, 4},
    {"Amp Attack", 2238, 102, true, 6, 2}, {"Amp Decay", 2291, 102, true, 6, 1},
    {"Amp Sustain", 2344, 102, true, 7, 5}, {"Amp Release", 2399, 102, true, 7, 7},
    {"Amp Amount", 2482, 75, false, 7, 6}, {"Amp LFO1 Depth", 2482, 162, false, 7, 4},
    {"LFO2 Rate", 76, 358, false, 1, 3}, {"LFO2 Depth", 157, 358, false, 1, 0}, {"Keyboard Time", 264, 358, false, 1, 1},
    {"Overdrive", 531, 483, false, 2, 3}, {"Chorus", 612, 483, false, 2, 0}, {"Delay", 692, 483, false, 2, 1}, {"Reverb", 772, 483, false, 2, 2},
    {"Arp Tempo", 878, 358, false, 3, 1}, {"Phrase Tempo", 1039, 357, false, 3, 2},
    {"EQ Low", 2105, 428, true, 6, 3}, {"EQ Freq", 2186, 428, true, 6, 0}, {"EQ B/C", 2239, 428, true, 7, 1},
    {"EQ Q", 2293, 428, true, 7, 0}, {"EQ High", 2373, 428, true, 7, 3}, {"Master Volume", 2483, 428, true, 7, 2},
};

// Panel C (service manual p41, "Panel C: Top Level"): the board left of the keyboard. The art has
// no drawing of it, so the panel view draws it in a column of its own left of LFO1: x below 0.
static const float panel_c_w = 240;

static const PanelKey panel_keys[] = {
    // Panel C buttons, bar 0 (BUTB[0]): BUT4 wheel mode, BUT5 aftertouch exchange, BUT3 velocity,
    // BUT1 aftertouch, BUT2 relative, BUT0 hold. Velocity and aftertouch measured by their pages.
    {"Wheel Mode", -170, 150, 0, 4}, {"Aftertouch Exchange", -68, 150, 0, 5},
    {"Velocity", -170, 340, 0, 3}, {"Aftertouch", -68, 340, 0, 1},
    {"Relative", -170, 560, 0, 2}, {"Hold", -68, 560, 0, 0},
    {"LFO1 Wave", 115, 256, 1, 7}, {"Common Osc", 582, 256, 1, 4},
    {"Osc1 Wave 1", 702, 256, 1, 3}, {"Osc1 Wave 2", 756, 256, 1, 2}, {"Osc1 Wave 3", 810, 256, 1, 1}, {"Osc1 PWM Source", 970, 256, 1, 0},
    // Measured against the lamps: BUT5 lights Osc2 Wave 1 (latch 7 bit 6), BUT4 Wave 2, BUT3 Wave 3.
    {"Osc2 Wave 1", 1211, 256, 5, 5}, {"Osc2 Wave 2", 1264, 256, 5, 4}, {"Osc2 Wave 3", 1318, 256, 5, 3},
    {"Sync", 1399, 75, 5, 7}, {"Osc2 PWM Source", 1477, 256, 5, 2}, {"Osc2 KBD Track", 1559, 256, 5, 1},
    {"Filter1", 1692, 256, 5, 0}, {"Routing", 1785, 164, 6, 6}, {"Filter2", 1785, 256, 5, 6}, {"Autopan", 2481, 256, 6, 7},
    {"LFO2 Wave", 73, 609, 3, 7}, {"LFO2 Depth Select", 152, 609, 3, 3},
    {"Portamento", 259, 485, 2, 4}, {"Unison", 340, 485, 2, 6}, {"Mono", 422, 359, 1, 6}, {"Legato", 421, 485, 2, 5},
    {"OCT-", 261, 609, 3, 4}, {"OCT+", 341, 609, 3, 6}, {"OSC SHIFT", 422, 609, 3, 5},
    {"Level/Parameter", 529, 359, 1, 5}, {"Overdrive On", 528, 609, 4, 5}, {"Chorus On", 607, 609, 4, 6}, {"Delay On", 690, 609, 4, 4}, {"Reverb On", 769, 609, 4, 3},
    {"Arp Hold", 876, 526, 2, 2}, {"Arp On", 876, 609, 4, 2}, {"Arp Mode", 955, 609, 4, 1}, {"Arp Range", 1035, 609, 4, 7},
    {"Phrase On", 956, 526, 2, 1}, {"Phrase Rec", 1036, 526, 2, 7},
    {"ESC", 1143, 505, 9, 0}, {"F1", 1240, 505, 9, 1}, {"F2", 1336, 505, 9, 2}, {"F3", 1430, 505, 9, 3}, {"F4", 1527, 505, 9, 4}, {"Disp Hold", 1623, 505, 9, 5},
    {"Split", 1196, 567, 10, 0}, {"Layer", 1248, 567, 10, 1}, {"Single", 1302, 567, 10, 2},
    {"Part 1", 1197, 608, 11, 0}, {"Part 2", 1249, 608, 11, 1}, {"Part 3", 1304, 608, 11, 2}, {"Part 4", 1356, 608, 11, 6},
    {"Program", 1464, 567, 10, 3}, {"Timbre", 1516, 567, 10, 4}, {"System", 1571, 567, 10, 5},
    {"Panel Scan", 1464, 608, 11, 3}, {"Routers", 1517, 608, 11, 4}, {"Write", 1571, 608, 11, 5},
    {"Up", 1755, 505, 9, 7}, {"Down", 1755, 567, 10, 6}, {"Left", 1724, 536, 9, 6}, {"Right", 1787, 536, 10, 7},
    {"7", 1892, 380, 8, 0}, {"8", 1943, 380, 6, 0}, {"9", 1997, 380, 7, 0},
    {"4", 1892, 422, 8, 1}, {"5", 1943, 422, 6, 1}, {"6", 1997, 422, 7, 1},
    {"1", 1892, 464, 8, 2}, {"2", 1943, 464, 6, 2}, {"3", 1997, 464, 7, 2},
    {"Bank", 1892, 506, 8, 3}, {"0", 1943, 506, 6, 3}, {"Enter", 1997, 506, 7, 3},
    {"Morph Auto", 1892, 568, 8, 4}, {"Morph Manual", 1943, 568, 6, 4}, {"Morph Enable", 1997, 568, 7, 4},
    {"Loc 1", 1892, 609, 8, 5}, {"Loc 2", 1944, 609, 6, 5}, {"Motion Rec", 1997, 609, 7, 5},
    {"EQ Mid Mode", 2240, 609, 7, 6}, {"EQ Bypass", 2373, 609, 7, 7},
};

static const float panel_dial_x = 1755, panel_dial_y = 380, panel_dial_r = 60;
static const float panel_lcd_x0 = 1190, panel_lcd_y0 = 325, panel_lcd_x1 = 1578, panel_lcd_y1 = 433;

// LEDs, from the service manual: Panel A (p36) and B (p40) each carry six 74HC595 on LED[1..6] and
// LED[7..12]; LED[n] is the strobe at 68FF80+n. The first bit shifted lands on Q7, so Q(k) is bit
// (7 - k) of the latch byte. Which DL sits where is from the PCB layouts (p46, p47). A lamp sits in a
// key; an indicator is a small LED beside a label.
struct PanelLed { const char* name; float x, y; int latch, bit; bool lamp; };
static const PanelLed panel_leds[] = {
    // Panel C lamps, latch 0 (its 74HC595): measured by pressing each key; M.Morph's bit is the one
    // left over, not seen lit (the wheel mode key did not switch in the tests).
    {"Stnd", -195, 100, 0, 5, false}, {"M.Morph", -145, 100, 0, 4, false},
    {"Aftertouch Exchange", -79, 150, 0, 7, true}, {"Velocity", -181, 340, 0, 3, true},
    {"Aftertouch", -79, 340, 0, 1, true}, {"Relative", -181, 560, 0, 6, true}, {"Hold", -79, 560, 0, 2, true},
    {"LFO2 Wave 1", 58, 485, 1, 7, false},
    {"LFO2 Wave 2", 58, 515, 1, 6, false},
    {"LFO2 Wave 3", 58, 545, 1, 5, false},
    {"LFO2 Wave 4", 58, 575, 1, 4, false},
    {"LFO2 to Osc1", 139, 485, 1, 3, false},
    {"LFO2 to Osc2", 139, 515, 1, 2, false},
    {"LFO2 to Filters", 139, 545, 1, 1, false},
    {"LFO2 to Amplifier", 139, 575, 1, 0, false},
    {"Mono", 411, 359, 2, 7, true},
    {"Oct -2", 246, 590, 2, 6, false},
    {"Oct -1", 275, 590, 2, 5, false},
    {"Oct +1", 326, 590, 2, 4, false},
    {"Oct +2", 356, 590, 2, 3, false},
    {"Shift Osc1", 406, 590, 2, 2, false},
    {"Shift Osc2", 436, 590, 2, 1, false},
    {"Legato", 411, 484, 2, 0, true},
    {"Portamento", 248, 484, 3, 7, true},
    {"Unison", 330, 484, 3, 6, true},
    {"Env Osc1", 566, 236, 3, 5, false},
    {"Env Osc2", 596, 236, 3, 4, false},
    {"LFO1 Wave 1", 69, 236, 3, 3, false},
    {"LFO1 Wave 2", 98, 236, 3, 2, false},
    {"LFO1 Wave 3", 128, 236, 3, 1, false},
    {"LFO1 Wave 4", 158, 236, 3, 0, false},
    {"Phrase On", 945, 526, 4, 7, true},
    {"Arp Up", 926, 590, 4, 6, false},
    {"Arp Down", 956, 590, 4, 5, false},
    {"Arp Rnd", 985, 590, 4, 4, false},
    {"Arp +1", 1018, 590, 4, 3, false},
    {"Arp +2", 1036, 590, 4, 2, false},
    {"Arp +3", 1054, 590, 4, 1, false},
    {"Phrase Rec", 1025, 526, 4, 0, true},
    {"Level/Parameter", 518, 359, 5, 7, true},
    {"Overdrive On", 517, 609, 5, 6, true},
    {"Chorus On", 597, 609, 5, 5, true},
    {"Delay On", 679, 609, 5, 4, true},
    {"Reverb On", 758, 609, 5, 3, true},
    {"Arp On", 865, 609, 5, 2, true},
    {"Arp Hold", 865, 526, 5, 1, true},
    {"Arp Tap Tempo", 865, 443, 5, 0, true},
    {"Phrase Tap Tempo", 1026, 443, 6, 7, true},
    {"Osc1 PWM LFO1", 939, 236, 6, 5, false},
    {"Osc1 PWM LFO2", 969, 236, 6, 4, false},
    {"Osc1 PWM EG", 999, 236, 6, 3, false},
    {"Osc1 Wave 3", 799, 256, 6, 2, true},
    {"Osc1 Wave 2", 745, 256, 6, 1, true},
    {"Osc1 Wave 1", 692, 256, 6, 0, true},
    {"Sync", 1388, 75, 7, 7, true},
    {"Osc2 Wave 1", 1200, 256, 7, 6, true},
    {"Osc2 Wave 2", 1253, 256, 7, 5, true},
    {"Osc2 Wave 3", 1307, 256, 7, 4, true},
    {"Osc2 PWM LFO1", 1447, 236, 7, 3, false},
    {"Osc2 PWM LFO2", 1477, 236, 7, 2, false},
    {"Osc2 PWM EG", 1507, 236, 7, 1, false},
    {"Osc2 KBD Track", 1548, 256, 7, 0, true},
    {"Filter2 HP", 1804, 236, 8, 6, false},
    {"Filter2 BP", 1785, 236, 8, 5, false},
    {"Filter2 LP", 1765, 236, 8, 4, false},
    {"Filter1 HP", 1710, 236, 8, 3, false},
    {"Filter1 LP", 1672, 236, 8, 2, false},
    {"Filter1 BP", 1691, 236, 8, 1, false},
    {"EQ Bypass", 2362, 609, 9, 7, true},
    {"Autopan", 2470, 256, 9, 6, true},
    {"Routing Serial", 1755, 145, 9, 5, false},
    {"Routing Par", 1785, 145, 9, 4, false},
    {"Routing Split", 1814, 145, 9, 3, false},
    {"Mid Graph", 2224, 590, 9, 2, false},
    {"Mid Param", 2254, 590, 9, 1, false},
    {"Enter", 1986, 505, 10, 7, true},
    {"Morph Auto", 1881, 568, 10, 6, true},
    {"Morph Manual", 1933, 568, 10, 5, true},
    {"Morph Enable", 1986, 568, 10, 4, true},
    {"Loc 1", 1882, 609, 10, 3, true},
    {"Loc 2", 1933, 609, 10, 2, true},
    {"Motion Rec", 1986, 609, 10, 1, true},
    {"Disp Hold", 1612, 505, 11, 7, true},
    {"Program", 1453, 567, 11, 6, true},
    {"Timbre", 1505, 567, 11, 5, true},
    {"System", 1560, 567, 11, 4, true},
    {"Write", 1560, 608, 11, 3, true},
    {"Panel Scan", 1454, 608, 11, 2, true},
    {"Routers", 1506, 608, 11, 1, true},
    {"Split", 1185, 567, 12, 6, true},
    {"Layer", 1237, 567, 12, 5, true},
    {"Single", 1292, 567, 12, 4, true},
    {"Part 1", 1186, 608, 12, 3, true},
    {"Part 2", 1238, 608, 12, 2, true},
    {"Part 3", 1293, 608, 12, 1, true},
    {"Part 4", 1346, 608, 12, 0, true},
};
