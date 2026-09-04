#include "palettes.h"

namespace effects {
namespace {

// A palette stop: color `r,g,b` at position `index` (0-255) around the
// palette. Every palette here - both the 7 FastLED named-color tables and
// the 59 WLED gradient tables - is normalized to this form so a single
// lookup routine (sample(), below) serves all of them.
struct Stop {
  uint8_t index, r, g, b;
};

// The 7 built-in FastLED palettes (WLED IDs 6-12). wled00/palettes.cpp's
// CloudColors_p/LavaColors_p/OceanColors_p/ForestColors_p (named CRGB::
// constants, resolved via FastLED's HTMLColorCode enum) and PartyColors_gc22/
// RainbowColors_gc22/RainbowStripeColors_gc22 (already raw hex, gamma-corrected
// in the source). Each is a 16-entry CRGBPalette16 - FastLED spaces those 16
// entries evenly across the 0-255 index range (entry i -> i*255/15 = i*17).
constexpr Stop kPartyStops[16] = {
  {  0,155,  0,213}, { 17,189,  0,184}, { 34,218,  0,146}, { 51,243,  0, 92},
  { 68,244, 85,  0}, { 85,220,143,  0}, {102,213,180,  0}, {119,213,213,  0},
  {136,213,155,  0}, {153,239,102,  0}, {170,249,  0, 68}, {187,225,  0,134},
  {204,196,  0,176}, {221,163,  0,207}, {238,118,  0,232}, {255,  0, 50,252},
};

constexpr Stop kCloudStops[16] = {
  {  0,  0,  0,255}, { 17,  0,  0,139}, { 34,  0,  0,139}, { 51,  0,  0,139},
  { 68,  0,  0,139}, { 85,  0,  0,139}, {102,  0,  0,139}, {119,  0,  0,139},
  {136,  0,  0,255}, {153,  0,  0,139}, {170,135,206,235}, {187,135,206,235},
  {204,173,216,230}, {221,255,255,255}, {238,173,216,230}, {255,135,206,235},
};

constexpr Stop kLavaStops[16] = {
  {  0,  0,  0,  0}, { 17,128,  0,  0}, { 34,  0,  0,  0}, { 51,128,  0,  0},
  { 68,139,  0,  0}, { 85,139,  0,  0}, {102,128,  0,  0}, {119,139,  0,  0},
  {136,139,  0,  0}, {153,139,  0,  0}, {170,255,  0,  0}, {187,255,165,  0},
  {204,255,255,255}, {221,255,165,  0}, {238,255,  0,  0}, {255,139,  0,  0},
};

constexpr Stop kOceanStops[16] = {
  {  0, 25, 25,112}, { 17,  0,  0,139}, { 34, 25, 25,112}, { 51,  0,  0,128},
  { 68,  0,  0,139}, { 85,  0,  0,205}, {102, 46,139, 87}, {119,  0,128,128},
  {136, 95,158,160}, {153,  0,  0,255}, {170,  0,139,139}, {187,100,149,237},
  {204,127,255,212}, {221, 46,139, 87}, {238,  0,255,255}, {255,135,206,250},
};

constexpr Stop kForestStops[16] = {
  {  0,  0,100,  0}, { 17,  0,100,  0}, { 34, 85,107, 47}, { 51,  0,100,  0},
  { 68,  0,128,  0}, { 85, 34,139, 34}, {102,107,142, 35}, {119,  0,128,  0},
  {136, 46,139, 87}, {153,102,205,170}, {170, 50,205, 50}, {187,154,205, 50},
  {204,144,238,144}, {221,124,252,  0}, {238,102,205,170}, {255, 34,139, 34},
};

constexpr Stop kRainbowStops[16] = {
  {  0,255,  0,  0}, { 17,235,112,  0}, { 34,213,155,  0}, { 51,213,186,  0},
  { 68,213,213,  0}, { 85,156,235,  0}, {102,  0,255,  0}, {119,  0,235,112},
  {136,  0,213,155}, {153,  0,156,212}, {170,  0,  0,255}, {187,112,  0,235},
  {204,155,  0,213}, {221,186,  0,187}, {238,213,  0,155}, {255,235,  0,114},
};

constexpr Stop kRainbowBandsStops[16] = {
  {  0,255,  0,  0}, { 17,  0,  0,  0}, { 34,213,155,  0}, { 51,  0,  0,  0},
  { 68,213,213,  0}, { 85,  0,  0,  0}, {102,  0,255,  0}, {119,  0,  0,  0},
  {136,  0,213,155}, {153,  0,  0,  0}, {170,  0,  0,255}, {187,  0,  0,  0},
  {204,155,  0,213}, {221,  0,  0,  0}, {238,213,  0,155}, {255,  0,  0,  0},
};

// The 59 built-in WLED gradient palettes (WLED IDs 13-71).
// wled00/palettes.cpp's gGradientPalettes[] entries, in that exact order -
// each DEFINE_GRADIENT_PALETTE-style table is already {index,r,g,b} stops.
// id 13 "Sunset" (Sunset_Real_gp)
constexpr Stop kGradSunsetRealStops[7] = {
  {  0,181,  0,  0}, { 22,218, 85,  0}, { 51,255,170,  0}, { 85,211, 85, 77},
  {135,167,  0,169}, {198, 73,  0,188}, {255,  0,  0,207},
};

// id 14 "Rivendell" (es_rivendell_15_gp)
constexpr Stop kGradEsRivendell15Stops[5] = {
  {  0, 24, 69, 44}, {101, 73,105, 70}, {165,129,140, 97}, {242,200,204,166},
  {255,200,204,166},
};

// id 15 "Breeze" (es_ocean_breeze_036_gp)
constexpr Stop kGradEsOceanBreeze036Stops[4] = {
  {  0, 16, 48, 51}, { 89, 27,166,175}, {153,197,233,255}, {255,  0,145,152},
};

// id 16 "Red & Blue" (rgi_15_gp)
constexpr Stop kGradRgi15Stops[9] = {
  {  0, 41, 14, 99}, { 31,128, 24, 74}, { 63,227, 34, 50}, { 95,132, 31, 76},
  {127, 47, 29,102}, {159,109, 47,101}, {191,176, 66,100}, {223,129, 57,104},
  {255, 84, 48,108},
};

// id 17 "Yellowout" (retro2_16_gp)
constexpr Stop kGradRetro216Stops[2] = {
  {  0,222,191,  8}, {255,117, 52,  1},
};

// id 18 "Analogous" (Analogous_1_gp)
constexpr Stop kGradAnalogous1Stops[5] = {
  {  0, 38,  0,255}, { 63, 86,  0,255}, {127,139,  0,255}, {191,196,  0,117},
  {255,255,  0,  0},
};

// id 19 "Splash" (es_pinksplash_08_gp)
constexpr Stop kGradEsPinksplash08Stops[5] = {
  {  0,186, 63,255}, {127,227,  9, 85}, {175,234,205,213}, {221,205, 38,176},
  {255,205, 38,176},
};

// id 20 "Pastel" (Sunset_Yellow_gp)
constexpr Stop kGradSunsetYellowStops[11] = {
  {  0, 61,135,184}, { 36,129,188,169}, { 87,203,241,155}, {100,228,237,141},
  {107,255,232,127}, {115,251,202,130}, {120,248,172,133}, {128,251,202,130},
  {180,255,232,127}, {223,255,242,120}, {255,255,252,113},
};

// id 21 "Sunset2" (Another_Sunset_gp)
constexpr Stop kGradAnotherSunsetStops[8] = {
  {  0,175,121, 62}, { 29,128,103, 60}, { 68, 84, 84, 58}, { 68,248,184, 55},
  { 97,239,204, 93}, {124,230,225,133}, {178,102,125,129}, {255,  0, 26,125},
};

// id 22 "Beech" (Beech_gp)
constexpr Stop kGradBeechStops[15] = {
  {  0,255,254,236}, { 12,255,254,236}, { 22,255,254,236}, { 26,223,224,178},
  { 28,192,195,124}, { 28,176,255,231}, { 50,123,251,236}, { 71, 74,246,241},
  { 93, 33,225,228}, {120,  0,204,215}, {133,  4,168,178}, {136, 10,132,143},
  {136, 51,189,212}, {208, 23,159,201}, {255,  0,129,190},
};

// id 23 "Vintage" (es_vintage_01_gp)
constexpr Stop kGradEsVintage01Stops[8] = {
  {  0, 41, 18, 24}, { 51, 73,  0, 22}, { 76,165,170, 38}, {101,255,189, 80},
  {127,139, 56, 40}, {153, 73,  0, 22}, {229, 41, 18, 24}, {255, 41, 18, 24},
};

// id 24 "Departure" (departure_gp)
constexpr Stop kGradDepartureStops[12] = {
  {  0, 53, 34,  0}, { 42, 86, 51,  0}, { 63,147,108, 49}, { 84,212,166,108},
  {106,235,212,180}, {116,255,255,255}, {138,191,255,193}, {148, 84,255, 88},
  {170,  0,255,  0}, {191,  0,192,  0}, {212,  0,128,  0}, {255,  0,128,  0},
};

// id 25 "Landscape" (es_landscape_64_gp)
constexpr Stop kGradEsLandscape64Stops[9] = {
  {  0,  0,  0,  0}, { 37, 31, 89, 19}, { 76, 72,178, 43}, {127,150,235,  5},
  {128,186,234,119}, {130,222,233,252}, {153,197,219,231}, {204,132,179,253},
  {255, 28,107,225},
};

// id 26 "Beach" (es_landscape_33_gp)
constexpr Stop kGradEsLandscape33Stops[6] = {
  {  0, 12, 45,  0}, { 19,101, 86,  2}, { 38,207,128,  4}, { 63,243,197, 18},
  { 66,109,196,146}, {255,  5, 39,  7},
};

// id 27 "Sherbet" (rainbowsherbet_gp)
constexpr Stop kGradRainbowsherbetStops[7] = {
  {  0,255,102, 41}, { 43,255,140, 90}, { 86,255, 51, 90}, {127,255,153,169},
  {170,255,255,249}, {209,113,255, 85}, {255,157,255,137},
};

// id 28 "Hult" (gr65_hult_gp)
constexpr Stop kGradGr65HultStops[6] = {
  {  0,251,216,252}, { 48,255,192,255}, { 89,239, 95,241}, {160, 51,153,217},
  {216, 24,184,174}, {255, 24,184,174},
};

// id 29 "Hult64" (gr64_hult_gp)
constexpr Stop kGradGr64HultStops[8] = {
  {  0, 24,184,174}, { 66,  8,162,150}, {104,124,137,  7}, {130,178,186, 22},
  {150,124,137,  7}, {201,  6,156,144}, {239,  0,128,117}, {255,  0,128,117},
};

// id 30 "Drywet" (GMT_drywet_gp)
constexpr Stop kGradGMTDrywetStops[7] = {
  {  0,119, 97, 33}, { 42,235,199, 88}, { 84,169,238,124}, {127, 37,238,232},
  {170,  7,120,236}, {212, 27,  1,175}, {255,  4, 51,101},
};

// id 31 "Jul" (ib_jul01_gp)
constexpr Stop kGradIbJul01Stops[4] = {
  {  0,226,  6, 12}, { 94, 26, 96, 78}, {132,130,189, 94}, {255,177,  3,  9},
};

// id 32 "Grintage" (es_vintage_57_gp)
constexpr Stop kGradEsVintage57Stops[5] = {
  {  0, 29,  8,  3}, { 53, 76,  1,  0}, {104,142, 96, 28}, {153,211,191, 61},
  {255,117,129, 42},
};

// id 33 "Rewhi" (ib15_gp)
constexpr Stop kGradIb15Stops[6] = {
  {  0,177,160,199}, { 72,205,158,149}, { 89,233,155,101}, {107,255, 95, 63},
  {141,192, 98,109}, {255,132,101,159},
};

// id 34 "Tertiary" (Tertiary_01_gp)
constexpr Stop kGradTertiary01Stops[5] = {
  {  0,  0, 25,255}, { 63, 38,140,117}, {127, 86,255,  0}, {191,167,140, 19},
  {255,255, 25, 41},
};

// id 35 "Fire" (lava_gp)
constexpr Stop kGradLavaStops[13] = {
  {  0,  0,  0,  0}, { 46, 77,  0,  0}, { 96,177,  0,  0}, {108,196, 38,  9},
  {119,215, 76, 19}, {146,235,115, 29}, {174,255,153, 41}, {188,255,178, 41},
  {202,255,204, 41}, {218,255,230, 41}, {234,255,255, 41}, {244,255,255,143},
  {255,255,255,255},
};

// id 36 "Icefire" (fierce_ice_gp)
constexpr Stop kGradFierceIceStops[7] = {
  {  0,  0,  0,  0}, { 59,  0, 51,117}, {119,  0,102,255}, {149, 38,153,255},
  {180, 86,204,255}, {217,167,230,255}, {255,255,255,255},
};

// id 37 "Cyane" (Colorfull_gp)
constexpr Stop kGradColorfullStops[11] = {
  {  0, 61,155, 44}, { 25, 95,174, 77}, { 60,132,193,113}, { 93,154,166,125},
  {106,175,138,136}, {109,183,121,137}, {113,194,104,138}, {116,225,179,165},
  {124,255,255,192}, {168,167,218,203}, {255, 84,182,215},
};

// id 38 "Light Pink" (Pink_Purple_gp)
constexpr Stop kGradPinkPurpleStops[11] = {
  {  0, 79, 32,109}, { 25, 90, 40,117}, { 51,102, 48,124}, { 76,141,135,185},
  {102,180,222,248}, {109,208,236,252}, {114,237,250,255}, {122,206,200,239},
  {149,177,149,222}, {183,187,130,203}, {255,198,111,184},
};

// id 39 "Autumn" (es_autumn_19_gp)
constexpr Stop kGradEsAutumn19Stops[13] = {
  {  0, 90, 14,  5}, { 51,139, 41, 13}, { 84,180, 70, 17}, {104,192,202,125},
  {112,177,137,  3}, {122,190,200,131}, {124,192,202,124}, {135,177,137,  3},
  {142,194,203,118}, {163,177, 68, 17}, {204,128, 35, 12}, {249, 74,  5,  2},
  {255, 74,  5,  2},
};

// id 40 "Magenta" (BlacK_Blue_Magenta_White_gp)
constexpr Stop kGradBlackBlueMagentaWhiteStops[7] = {
  {  0,  0,  0,  0}, { 42,  0,  0,117}, { 84,  0,  0,255}, {127,113,  0,255},
  {170,255,  0,255}, {212,255,128,255}, {255,255,255,255},
};

// id 41 "Magred" (BlacK_Magenta_Red_gp)
constexpr Stop kGradBlackMagentaRedStops[5] = {
  {  0,  0,  0,  0}, { 63,113,  0,117}, {127,255,  0,255}, {191,255,  0,117},
  {255,255,  0,  0},
};

// id 42 "Yelmag" (BlacK_Red_Magenta_Yellow_gp)
constexpr Stop kGradBlackRedMagentaYellowStops[7] = {
  {  0,  0,  0,  0}, { 42,113,  0,  0}, { 84,255,  0,  0}, {127,255,  0,117},
  {170,255,  0,255}, {212,255,128,117}, {255,255,255,  0},
};

// id 43 "Yelblu" (Blue_Cyan_Yellow_gp)
constexpr Stop kGradBlueCyanYellowStops[5] = {
  {  0,  0,  0,255}, { 63,  0,128,255}, {127,  0,255,255}, {191,113,255,117},
  {255,255,255,  0},
};

// id 44 "Orange & Teal" (Orange_Teal_gp)
constexpr Stop kGradOrangeTealStops[4] = {
  {  0,  0,150, 92}, { 55,  0,150, 92}, {200,255, 72,  0}, {255,255, 72,  0},
};

// id 45 "Tiamat" (Tiamat_gp)
constexpr Stop kGradTiamatStops[11] = {
  {  0,  1,  2, 14}, { 33,  2,  5, 35}, {100, 13,135, 92}, {120, 43,255,193},
  {140,247,  7,249}, {160,193, 17,208}, {180, 39,255,154}, {200,  4,213,236},
  {220, 39,252,135}, {240,193,213,253}, {255,255,249,255},
};

// id 46 "April Night" (April_Night_gp)
constexpr Stop kGradAprilNightStops[17] = {
  {  0,  1,  5, 45}, { 10,  1,  5, 45}, { 25,  5,169,175}, { 40,  1,  5, 45},
  { 61,  1,  5, 45}, { 76, 45,175, 31}, { 91,  1,  5, 45}, {112,  1,  5, 45},
  {127,249,150,  5}, {143,  1,  5, 45}, {162,  1,  5, 45}, {178,255, 92,  0},
  {193,  1,  5, 45}, {214,  1,  5, 45}, {229,223, 45, 72}, {244,  1,  5, 45},
  {255,  1,  5, 45},
};

// id 47 "Orangery" (Orangery_gp)
constexpr Stop kGradOrangeryStops[9] = {
  {  0,255, 95, 23}, { 30,255, 82,  0}, { 60,223, 13,  8}, { 90,144, 44,  2},
  {120,255,110, 17}, {150,255, 69,  0}, {180,158, 13, 11}, {210,241, 82, 17},
  {255,213, 37,  4},
};

// id 48 "C9" (C9_gp)
constexpr Stop kGradC9Stops[8] = {
  {  0,184,  4,  0}, { 60,184,  4,  0}, { 65,144, 44,  2}, {125,144, 44,  2},
  {130,  4, 96,  2}, {190,  4, 96,  2}, {195,  7,  7, 88}, {255,  7,  7, 88},
};

// id 49 "Sakura" (Sakura_gp)
constexpr Stop kGradSakuraStops[5] = {
  {  0,196, 19, 10}, { 65,255, 69, 45}, {130,223, 45, 72}, {195,255, 82,103},
  {255,223, 13, 17},
};

// id 50 "Aurora" (Aurora_gp)
constexpr Stop kGradAuroraStops[6] = {
  {  0,  1,  5, 45}, { 64,  0,200, 23}, {128,  0,255,  0}, {170,  0,243, 45},
  {200,  0,135,  7}, {255,  1,  5, 45},
};

// id 51 "Atlantica" (Atlantica_gp)
constexpr Stop kGradAtlanticaStops[6] = {
  {  0,  0, 28,112}, { 50, 32, 96,255}, {100,  0,243, 45}, {150, 12, 95, 82},
  {200, 25,190, 95}, {255, 40,170, 80},
};

// id 52 "C9 2" (C9_2_gp)
constexpr Stop kGradC92Stops[10] = {
  {  0,  6,126,  2}, { 45,  6,126,  2}, { 46,  4, 30,114}, { 90,  4, 30,114},
  { 91,255,  5,  0}, {135,255,  5,  0}, {136,196, 57,  2}, {180,196, 57,  2},
  {181,137, 85,  2}, {255,137, 85,  2},
};

// id 53 "C9 New" (C9_new_gp)
constexpr Stop kGradC9NewStops[8] = {
  {  0,255,  5,  0}, { 60,255,  5,  0}, { 61,196, 57,  2}, {120,196, 57,  2},
  {121,  6,126,  2}, {180,  6,126,  2}, {181,  4, 30,114}, {255,  4, 30,114},
};

// id 54 "Temperature" (temperature_gp)
constexpr Stop kGradTemperatureStops[18] = {
  {  0, 20, 92,171}, { 14, 15,111,186}, { 28,  6,142,211}, { 42,  2,161,227},
  { 56, 16,181,239}, { 70, 38,188,201}, { 84, 86,204,200}, { 99,139,219,176},
  {113,182,229,125}, {127,196,230, 63}, {141,241,240, 22}, {155,254,222, 30},
  {170,251,199,  4}, {184,247,157,  9}, {198,243,114, 15}, {226,213, 30, 29},
  {240,151, 38, 35}, {255,151, 38, 35},
};

// id 55 "Aurora 2" (Aurora2_gp)
constexpr Stop kGradAurora2Stops[5] = {
  {  0, 17,177, 13}, { 64,121,242,  5}, {128, 25,173,121}, {192,250, 77,127},
  {255,171,101,221},
};

// id 56 "Retro Clown" (retro_clown_gp)
constexpr Stop kGradRetroClownStops[3] = {
  {  0,242,168, 38}, {117,226, 78, 80}, {255,161, 54,225},
};

// id 57 "Candy" (candy_gp)
constexpr Stop kGradCandyStops[5] = {
  {  0,243,242, 23}, { 15,242,168, 38}, {142,111, 21,151}, {198, 74, 22,150},
  {255,  0,  0,117},
};

// id 58 "Toxy Reaf" (toxy_reaf_gp)
constexpr Stop kGradToxyReafStops[2] = {
  {  0,  2,239,126}, {255,145, 35,217},
};

// id 59 "Fairy Reaf" (fairy_reaf_gp)
constexpr Stop kGradFairyReafStops[4] = {
  {  0,220, 19,187}, {160, 12,225,219}, {219,203,242,223}, {255,255,255,255},
};

// id 60 "Semi Blue" (semi_blue_gp)
constexpr Stop kGradSemiBlueStops[9] = {
  {  0,  0,  0,  0}, { 12, 24,  4, 38}, { 53, 55,  8, 84}, { 80, 43, 48,159},
  {119, 31, 89,237}, {145, 50, 59,166}, {186, 71, 30, 98}, {233, 31, 15, 45},
  {255,  0,  0,  0},
};

// id 61 "Pink Candy" (pink_candy_gp)
constexpr Stop kGradPinkCandyStops[7] = {
  {  0,255,255,255}, { 45, 50, 64,255}, {112,242, 16,186}, {140,255,255,255},
  {155,242, 16,186}, {196,116, 13,166}, {255,255,255,255},
};

// id 62 "Red Reaf" (red_reaf_gp)
constexpr Stop kGradRedReafStops[4] = {
  {  0, 36, 68,114}, {104,149,195,248}, {188,255,  0,  0}, {255, 94, 14,  9},
};

// id 63 "Aqua Flash" (aqua_flash_gp)
constexpr Stop kGradAquaFlashStops[7] = {
  {  0,  0,  0,  0}, { 66,130,242,245}, { 96,255,255, 53}, {124,255,255,255},
  {153,255,255, 53}, {188,130,242,245}, {255,  0,  0,  0},
};

// id 64 "Yelblu Hot" (yelblu_hot_gp)
constexpr Stop kGradYelbluHotStops[7] = {
  {  0, 43, 30, 57}, { 58, 73,  0,119}, {122, 87,  0, 74}, {158,197, 57, 22},
  {183,218,117, 27}, {219,239,177, 32}, {255,246,247, 27},
};

// id 65 "Lite Light" (lite_light_gp)
constexpr Stop kGradLiteLightStops[6] = {
  {  0,  0,  0,  0}, {  9, 20, 21, 22}, { 40, 46, 43, 49}, { 66, 46, 43, 49},
  {101, 61, 16, 65}, {255,  0,  0,  0},
};

// id 66 "Red Flash" (red_flash_gp)
constexpr Stop kGradRedFlashStops[5] = {
  {  0,  0,  0,  0}, { 99,242, 12,  8}, {130,253,228,163}, {155,242, 12,  8},
  {255,  0,  0,  0},
};

// id 67 "Blink Red" (blink_red_gp)
constexpr Stop kGradBlinkRedStops[8] = {
  {  0,  4,  7,  4}, { 43, 40, 25, 62}, { 76, 61, 15, 36}, {109,207, 39, 96},
  {127,255,156,184}, {165,185, 73,207}, {204,105, 66,240}, {255, 77, 29, 78},
};

// id 68 "Red Shift" (red_shift_gp)
constexpr Stop kGradRedShiftStops[7] = {
  {  0, 98, 22, 93}, { 45,103, 22, 73}, { 99,192, 45, 56}, {132,235,187, 59},
  {175,228, 85, 26}, {201,228, 56, 48}, {255,  2,  0,  2},
};

// id 69 "Red Tide" (red_tide_gp)
constexpr Stop kGradRedTideStops[11] = {
  {  0,251, 46,  0}, { 28,255,139, 25}, { 43,246,158, 63}, { 58,246,216,123},
  { 84,243, 94, 10}, {114,177, 65, 11}, {140,255,241,115}, {168,177, 65, 11},
  {196,250,233,158}, {216,255, 94,  6}, {255,126,  8,  4},
};

// id 70 "Candy2" (candy2_gp)
constexpr Stop kGradCandy2Stops[10] = {
  {  0,109,102,102}, { 25, 42, 49, 71}, { 48,121, 96, 84}, { 73,241,214, 26},
  { 89,216,104, 44}, {130, 42, 49, 71}, {163,255,177, 47}, {186,241,214, 26},
  {211,109,102,102}, {255, 20, 19, 13},
};

// id 71 "Traffic Light" (trafficlight_gp)
constexpr Stop kGradTrafficlightStops[4] = {
  {  0,  0,  0,  0}, { 85,  0,255,  0}, {170,255,255,  0}, {255,255,  0,  0},
};

struct PaletteTable {
  const Stop *stops;
  uint8_t count;
};

// index 0 = WLED id 6 (Party) ... index 6 = WLED id 12 (Rainbow Bands)
constexpr PaletteTable kFastledPalettes[7] = {
  {kPartyStops, 16},  // 6 Party
  {kCloudStops, 16},  // 7 Cloud
  {kLavaStops, 16},  // 8 Lava
  {kOceanStops, 16},  // 9 Ocean
  {kForestStops, 16},  // 10 Forest
  {kRainbowStops, 16},  // 11 Rainbow
  {kRainbowBandsStops, 16},  // 12 Rainbow Bands
};

// index 0 = WLED id 13 (Sunset) ... index 58 = WLED id 71 (Traffic Light)
constexpr PaletteTable kGradientPalettes[59] = {
  {kGradSunsetRealStops, 7},  // 13 Sunset
  {kGradEsRivendell15Stops, 5},  // 14 Rivendell
  {kGradEsOceanBreeze036Stops, 4},  // 15 Breeze
  {kGradRgi15Stops, 9},  // 16 Red & Blue
  {kGradRetro216Stops, 2},  // 17 Yellowout
  {kGradAnalogous1Stops, 5},  // 18 Analogous
  {kGradEsPinksplash08Stops, 5},  // 19 Splash
  {kGradSunsetYellowStops, 11},  // 20 Pastel
  {kGradAnotherSunsetStops, 8},  // 21 Sunset2
  {kGradBeechStops, 15},  // 22 Beech
  {kGradEsVintage01Stops, 8},  // 23 Vintage
  {kGradDepartureStops, 12},  // 24 Departure
  {kGradEsLandscape64Stops, 9},  // 25 Landscape
  {kGradEsLandscape33Stops, 6},  // 26 Beach
  {kGradRainbowsherbetStops, 7},  // 27 Sherbet
  {kGradGr65HultStops, 6},  // 28 Hult
  {kGradGr64HultStops, 8},  // 29 Hult64
  {kGradGMTDrywetStops, 7},  // 30 Drywet
  {kGradIbJul01Stops, 4},  // 31 Jul
  {kGradEsVintage57Stops, 5},  // 32 Grintage
  {kGradIb15Stops, 6},  // 33 Rewhi
  {kGradTertiary01Stops, 5},  // 34 Tertiary
  {kGradLavaStops, 13},  // 35 Fire
  {kGradFierceIceStops, 7},  // 36 Icefire
  {kGradColorfullStops, 11},  // 37 Cyane
  {kGradPinkPurpleStops, 11},  // 38 Light Pink
  {kGradEsAutumn19Stops, 13},  // 39 Autumn
  {kGradBlackBlueMagentaWhiteStops, 7},  // 40 Magenta
  {kGradBlackMagentaRedStops, 5},  // 41 Magred
  {kGradBlackRedMagentaYellowStops, 7},  // 42 Yelmag
  {kGradBlueCyanYellowStops, 5},  // 43 Yelblu
  {kGradOrangeTealStops, 4},  // 44 Orange & Teal
  {kGradTiamatStops, 11},  // 45 Tiamat
  {kGradAprilNightStops, 17},  // 46 April Night
  {kGradOrangeryStops, 9},  // 47 Orangery
  {kGradC9Stops, 8},  // 48 C9
  {kGradSakuraStops, 5},  // 49 Sakura
  {kGradAuroraStops, 6},  // 50 Aurora
  {kGradAtlanticaStops, 6},  // 51 Atlantica
  {kGradC92Stops, 10},  // 52 C9 2
  {kGradC9NewStops, 8},  // 53 C9 New
  {kGradTemperatureStops, 18},  // 54 Temperature
  {kGradAurora2Stops, 5},  // 55 Aurora 2
  {kGradRetroClownStops, 3},  // 56 Retro Clown
  {kGradCandyStops, 5},  // 57 Candy
  {kGradToxyReafStops, 2},  // 58 Toxy Reaf
  {kGradFairyReafStops, 4},  // 59 Fairy Reaf
  {kGradSemiBlueStops, 9},  // 60 Semi Blue
  {kGradPinkCandyStops, 7},  // 61 Pink Candy
  {kGradRedReafStops, 4},  // 62 Red Reaf
  {kGradAquaFlashStops, 7},  // 63 Aqua Flash
  {kGradYelbluHotStops, 7},  // 64 Yelblu Hot
  {kGradLiteLightStops, 6},  // 65 Lite Light
  {kGradRedFlashStops, 5},  // 66 Red Flash
  {kGradBlinkRedStops, 8},  // 67 Blink Red
  {kGradRedShiftStops, 7},  // 68 Red Shift
  {kGradRedTideStops, 11},  // 69 Red Tide
  {kGradCandy2Stops, 10},  // 70 Candy2
  {kGradTrafficlightStops, 4},  // 71 Traffic Light
};

// WLED's ColorFromPalette() linear-interpolates between the two CRGBPalette
// entries an index falls between; this does the same directly against our
// {index,r,g,b} stop tables. A deliberate simplification - it doesn't track
// FastLED's internal blended-CRGBPalette16 lookup quirks, only its two-stop
// linear blend - matching effects.cpp's breathe()/sin16_t note above for
// another intentional simplification in this codebase.
Rgb sample(const Stop *stops, int count, uint8_t index) {
  int i = 0;
  while (i < count - 2 && index > stops[i + 1].index) i++;
  const Stop &a = stops[i];
  const Stop &b = stops[i + 1];
  int span = b.index - a.index;
  if (span == 0) return Rgb{a.r, a.g, a.b};
  int pos = index - a.index;
  return Rgb{
      static_cast<uint8_t>(a.r + (static_cast<int>(b.r) - a.r) * pos / span),
      static_cast<uint8_t>(a.g + (static_cast<int>(b.g) - a.g) * pos / span),
      static_cast<uint8_t>(a.b + (static_cast<int>(b.b) - a.b) * pos / span),
  };
}

// wled00/src/dependencies/fastled_slim/fastled_slim.cpp fill_gradient_RGB():
// fills entries[start..end] (inclusive) with a linear blend from c1 to c2,
// using the same Q16.16 fixed-point step FastLED uses.
void fill_gradient(Rgb *entries, int start, Rgb c1, int end, Rgb c2) {
  if (end < start) {
    int t = start;
    start = end;
    end = t;
    Rgb tc = c1;
    c1 = c2;
    c2 = tc;
  }
  int rdist = static_cast<int>(c2.r) - c1.r;
  int gdist = static_cast<int>(c2.g) - c1.g;
  int bdist = static_cast<int>(c2.b) - c1.b;
  int divisor = end - start;
  if (divisor == 0) divisor = 1;
  int rdelta = (rdist << 16) / divisor;
  int gdelta = (gdist << 16) / divisor;
  int bdelta = (bdist << 16) / divisor;
  int rshift = c1.r << 16, gshift = c1.g << 16, bshift = c1.b << 16;
  for (int i = start; i <= end; i++) {
    entries[i] = Rgb{static_cast<uint8_t>(rshift >> 16),
                      static_cast<uint8_t>(gshift >> 16),
                      static_cast<uint8_t>(bshift >> 16)};
    rshift += rdelta;
    gshift += gdelta;
    bshift += bdelta;
  }
}

// wled00/FX_fcn.cpp:226 Segment::loadPalette(), cases 2-5: builds a 16-entry
// CRGBPalette16 straight from the segment's colors. Ported via the same
// fill_solid_RGB()/fill_gradient_RGB() calls the real cases use (2: solid;
// 3: 4-color gradient CRGBPalette16(prim,prim,sec,sec), which fastled_slim's
// fill_gradient_RGB splits into thirds - the two prim/prim and sec/sec
// thirds are degenerate (flat); 4: 3-color gradient CRGBPalette16(ter,sec,
// prim), split into halves; 5: two literal 16-entry sequences depending on
// whether tertiary is black, per CRGB's `operator bool` (r||g||b)).
void build_dynamic_palette(uint8_t pal, Rgb primary, Rgb secondary, Rgb tertiary, Rgb entries[16]) {
  switch (pal) {
    case 2:
      for (int i = 0; i < 16; i++) entries[i] = primary;
      break;
    case 3:
      fill_gradient(entries, 0, primary, 5, primary);
      fill_gradient(entries, 5, primary, 10, secondary);
      fill_gradient(entries, 10, secondary, 15, secondary);
      break;
    case 4:
      fill_gradient(entries, 0, tertiary, 8, secondary);
      fill_gradient(entries, 8, secondary, 15, primary);
      break;
    case 5:
    default:
      if (tertiary.r || tertiary.g || tertiary.b) {
        const Rgb seq[16] = {
            primary,   primary,   primary,   primary,   primary,
            secondary, secondary, secondary, secondary, secondary,
            tertiary,  tertiary,  tertiary,  tertiary,  tertiary,
            primary,
        };
        for (int i = 0; i < 16; i++) entries[i] = seq[i];
      } else {
        for (int i = 0; i < 8; i++) entries[i] = primary;
        for (int i = 8; i < 16; i++) entries[i] = secondary;
      }
      break;
  }
}

Rgb sample_dynamic(uint8_t pal, uint8_t index, Rgb primary, Rgb secondary, Rgb tertiary) {
  Rgb entries[16];
  build_dynamic_palette(pal, primary, secondary, tertiary, entries);
  Stop stops[16];
  for (int i = 0; i < 16; i++) {
    stops[i] = Stop{static_cast<uint8_t>(i * 17), entries[i].r, entries[i].g, entries[i].b};
  }
  return sample(stops, 16, index);
}

// wled00/FX_fcn.cpp:2212 JSON_palette_names - the exact strings /json/pal
// serves (including the "* " prefix real WLED uses to mark the four
// segment-color-derived palettes, ids 1-5). Note these don't always match
// the "//NN-MM Name" comments next to the table entries in palettes.cpp
// itself - e.g. ids 22/26 "Beach"/"Beech" are swapped between the two -
// JSON_palette_names is what real WLED's API actually returns, so that is
// what's ported here.
const char *const kPaletteNames[kPaletteCount] = {
    "Default", "* Random Cycle", "* Color 1", "* Colors 1&2", "* Color Gradient", "* Colors Only",
    "Party", "Cloud", "Lava", "Ocean",
    "Forest", "Rainbow", "Rainbow Bands", "Sunset", "Rivendell", "Breeze", "Red & Blue", "Yellowout", "Analogous", "Splash",
    "Pastel", "Sunset 2", "Beach", "Vintage", "Departure", "Landscape", "Beech", "Sherbet", "Hult", "Hult 64",
    "Drywet", "Jul", "Grintage", "Rewhi", "Tertiary", "Fire", "Icefire", "Cyane", "Light Pink", "Autumn",
    "Magenta", "Magred", "Yelmag", "Yelblu", "Orange & Teal", "Tiamat", "April Night", "Orangery", "C9", "Sakura",
    "Aurora", "Atlantica", "C9 2", "C9 New", "Temperature", "Aurora 2", "Retro Clown", "Candy", "Toxy Reaf", "Fairy Reaf",
    "Semi Blue", "Pink Candy", "Red Reaf", "Aqua Flash", "Yelblu Hot", "Lite Light", "Red Flash", "Blink Red", "Red Shift", "Red Tide",
    "Candy2", "Traffic Light",
};

}  // namespace

const char *palette_name(uint8_t id) {
  if (id >= kPaletteCount) id = 0;
  return kPaletteNames[id];
}

// wled00/FX_fcn.cpp:226 Segment::loadPalette() + FX.cpp's ColorFromPalette()
// callers, folded into one lookup: resolve `palette_id` to a color table (or
// segment-color construction for 2-5), then sample() it at `index`.
Rgb color_from_palette(uint8_t palette_id, uint8_t index, Rgb primary, Rgb secondary, Rgb tertiary) {
  uint8_t pal = palette_id;
  if (pal >= kPaletteCount) pal = 0;  // wled00/FX_fcn.cpp:239, out-of-range falls back to 0
  if (pal == 0) pal = 6;              // Default -> PartyColors, see palettes.h
  if (pal == 1) pal = 11;             // Random Cycle -> Rainbow, see palettes.h

  if (pal <= 5) return sample_dynamic(pal, index, primary, secondary, tertiary);
  if (pal <= 12) {
    const PaletteTable &t = kFastledPalettes[pal - 6];
    return sample(t.stops, t.count, index);
  }
  const PaletteTable &t = kGradientPalettes[pal - 13];
  return sample(t.stops, t.count, index);
}

}  // namespace effects
