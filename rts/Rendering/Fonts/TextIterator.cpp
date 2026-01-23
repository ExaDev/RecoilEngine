#include "TextIterator.hpp"

#include "FontHandler.h"
#include "FontConstants.hpp"
#include "System/StringUtil.h"
#include "System/Misc/TracyDefs.h"

void TextIterator::Execute()
{
    RECOIL_DETAILED_TRACY_ZONE;
    const int end = static_cast<int>(text.length());

    while (currentPos < end) {
        const int startPos = currentPos;
        const char32_t ch = utf8::GetNextChar(text, currentPos);

        CharEvent event {
            .startIdx = startPos,
            .endIdx = currentPos,
            .linesSkipped = 0,
            .value = ch
        };

        switch (ch) {

        case OldColorCodeIndicator: {
            if (fontHandler.disableOldColorIndicators) {
                // Treat as printable character
                if (!handler.OnPrintable(event))
                    return;
                break;
            }
            [[fallthrough]];
        }
        case ColorCodeIndicator: {
            event.endIdx = std::min(currentPos + 3, end); // 3 more bytes: R, G, B
            currentPos = event.endIdx;
            if (ExtractColor(event) && !handler.OnColorCode(event))
                return;
        } break;

        case OldColorCodeIndicatorEx: {
            if (fontHandler.disableOldColorIndicators) {
                // Treat as printable character
                if (!handler.OnPrintable(event))
                    return;
                break;
            }
            [[fallthrough]];
        }
        case ColorCodeIndicatorEx: {
            event.endIdx = std::min(currentPos + 8, end); // 8 more bytes: R,G,B,A,R,G,B,A
            currentPos = event.endIdx;
            if (ExtractColorEx(event) && !handler.OnColorCodeEx(event))
                return;
        } break;

        case ColorResetIndicator: {
            // Already advanced by GetNextChar
            if (!handler.OnColorReset(event))
                return;
        } break;

        case CR: {
            // Check for CRLF sequence
            if (currentPos < end && text[currentPos] == LF) {
                currentPos++; // Skip the LF
            }
            [[fallthrough]];
        }
        case LF: {
            skippedLines++;
            event.linesSkipped = skippedLines;
            if (!handler.OnNewline(event)) {
                skippedLines = 0;
                return;
            }
            skippedLines = 0; // Reset after processing
        } break;

        case spaceUTF16: {
            // Already advanced by GetNextChar
            if (!handler.OnSpace(event))
                return;
        } break;

        default: {
            // Printable character - already advanced
            if (!handler.OnPrintable(event))
                return;
        } break;
        }
    }

    handler.OnEnd();
}

bool TextIterator::ExtractColor(CharEvent& e) const
{
    if (e.endIdx - e.startIdx != 1 + 3) {
        return false;
    }

    e.value = SColor {
        static_cast<uint8_t>(text[e.endIdx - 3]),
        static_cast<uint8_t>(text[e.endIdx - 2]),
        static_cast<uint8_t>(text[e.endIdx - 1]),
        static_cast<uint8_t>(255)
    };
    return true;
}
bool TextIterator::ExtractColorEx(CharEvent& e) const
{
    if (e.endIdx - e.startIdx != 1 + 4 + 4) {
        return false;
    }

    e.value = FontColors {
        .textColor = SColor {
            static_cast<uint8_t>(text[e.endIdx - 4]),
            static_cast<uint8_t>(text[e.endIdx - 3]),
            static_cast<uint8_t>(text[e.endIdx - 2]),
            static_cast<uint8_t>(text[e.endIdx - 1])
        },
        .outlineColor = SColor {
            static_cast<uint8_t>(text[e.endIdx - 8]),
            static_cast<uint8_t>(text[e.endIdx - 7]),
            static_cast<uint8_t>(text[e.endIdx - 6]),
            static_cast<uint8_t>(text[e.endIdx - 5])
        }
    };
    return true;
}