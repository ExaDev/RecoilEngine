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
        const char32_t ch = utf8::GetNextChar(text, currentPos); // ADVANCE immediately

        CharEvent event{ CharType::Printable, startPos, currentPos, ch, 0 };

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
            event.type = CharType::ColorCode;
            event.endIdx = std::min(currentPos + 3, end); // 3 more bytes: R, G, B
            currentPos = event.endIdx;
            if (!handler.OnColorCode(event, text))
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
            event.type = CharType::ColorCodeEx;
            event.endIdx = std::min(currentPos + 8, end); // 8 more bytes: R,G,B,A,R,G,B,A
            currentPos = event.endIdx;
            if (!handler.OnColorCodeEx(event, text))
                return;
        } break;

        case ColorResetIndicator: {
            event.type = CharType::ColorReset;
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
            event.type = CharType::Newline;
            skippedLines++;
            event.linesSkipped = skippedLines;
            if (!handler.OnNewline(event)) {
                skippedLines = 0;
                return;
            }
            skippedLines = 0; // Reset after processing
        } break;

        case spaceUTF16: {
            event.type = CharType::Space;
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