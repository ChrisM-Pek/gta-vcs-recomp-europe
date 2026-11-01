// The look: Vice City after sunset. A deep violet night, one warm sunset gradient for the thing to do next, teal for
// done, amber and red for attention. Every colour the UI draws comes from here; sizes are logical units.
#pragma once

#include "core/math.h"

namespace theme
{
    using core::Color;
    using core::rgb;

    // surfaces, darkest first
    constexpr Color kNight = rgb(0x0C0A12);    // behind everything
    constexpr Color kBase = rgb(0x13101B);
    constexpr Color kSurface = rgb(0x1B1626);  // cards
    constexpr Color kSurfaceHi = rgb(0x241D33); // hovered cards, controls
    constexpr Color kSurfaceTop = rgb(0x2D2540);
    constexpr Color kLine = rgb(0x2F2843);
    constexpr Color kLineSoft = rgb(0xFFFFFF, 14);

    // text
    constexpr Color kText = rgb(0xF3EFF9);
    constexpr Color kDim = rgb(0xA79FBA);
    constexpr Color kFaint = rgb(0x6F6784);

    // signal colours
    constexpr Color kPink = rgb(0xFF4F8E);
    constexpr Color kSunset = rgb(0xFF8A4C); // the primary gradient's other end
    constexpr Color kTeal = rgb(0x3DDBC6);
    constexpr Color kAmber = rgb(0xF5B544);
    constexpr Color kRed = rgb(0xF26868);
    constexpr Color kViolet = rgb(0x8E74FF);

    constexpr Color kShadow = rgb(0x05030A, 170);
    constexpr Color kScrim = rgb(0x07050C, 175); // behind dialogs and sheets

    // type sizes
    constexpr float kWordmark = 46;
    constexpr float kTitle = 30;
    constexpr float kHeading = 19;
    constexpr float kBody = 14.5f;
    constexpr float kSmall = 13;
    constexpr float kEyebrow = 11.5f;

    // shape
    constexpr float kRadius = 12;      // cards
    constexpr float kRadiusSmall = 8;  // buttons, controls
    constexpr float kTitleBar = 44;

    // Segoe Fluent Icons / Segoe MDL2 Assets code points
    namespace icon
    {
        constexpr char32_t kPlay = 0xE768;
        constexpr char32_t kSettings = 0xE713;
        constexpr char32_t kHelp = 0xE897;
        constexpr char32_t kSave = 0xE74E;
        constexpr char32_t kFolder = 0xED25;   // an open folder
        constexpr char32_t kOpenFile = 0xE8E5; // a page with an arrow: choosing a file
        constexpr char32_t kDisc = 0xE958;
        constexpr char32_t kDownload = 0xE896;
        constexpr char32_t kRefresh = 0xE72C;
        constexpr char32_t kMinimize = 0xE921;
        constexpr char32_t kMaximize = 0xE922;
        constexpr char32_t kRestore = 0xE923;
        constexpr char32_t kClose = 0xE8BB;
        constexpr char32_t kCheck = 0xE73E;
        constexpr char32_t kCancel = 0xE711;
        constexpr char32_t kWarning = 0xE7BA;
        constexpr char32_t kInfo = 0xE946;
        constexpr char32_t kBack = 0xE72B;
        constexpr char32_t kChevronRight = 0xE76C;
        constexpr char32_t kChevronDown = 0xE70D;
        constexpr char32_t kDocument = 0xE8A5;
        constexpr char32_t kRepair = 0xE90F;
        constexpr char32_t kCode = 0xE943;
        constexpr char32_t kChip = 0xE950;
        constexpr char32_t kClock = 0xE823;
        constexpr char32_t kSync = 0xE895;
        constexpr char32_t kLink = 0xE71B;
        constexpr char32_t kUndo = 0xE7A7;
        constexpr char32_t kBug = 0xEBE8;
        constexpr char32_t kDrive = 0xEDA2;
        constexpr char32_t kOpen = 0xE8A7; // open in new window
        constexpr char32_t kList = 0xEA37;
        constexpr char32_t kGame = 0xE7FC;
        constexpr char32_t kStop = 0xE71A;
    }
}
