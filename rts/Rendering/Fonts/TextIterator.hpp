#pragma once

#include "ustring.h"



enum class CharType {
    Space,
    Printable,
    Newline,
    ColorCode,
    ColorCodeEx,
    ColorReset
};

struct CharEvent {
    CharType type;
    int startIdx;
    int endIdx;
    char32_t codepoint;
    int linesSkipped = 0;
};

/*
* Duck typing, but too much boilerplate to implement not required methods?
* Go for virtual / override approach instead
template<typename T>
concept TextIteratorHandler = requires(T & handler, const CharEvent & e, const spring::u8string & text) {
    { handler.OnColorCode(e, text) } -> std::same_as<bool>;
    { handler.OnColorCodeEx(e, text) } -> std::same_as<bool>;
    { handler.OnColorReset(e) } -> std::same_as<bool>;
    { handler.OnNewline(e) } -> std::same_as<bool>;
    { handler.OnPrintable(e) } -> std::same_as<bool>;
    { handler.OnSpace(e) } -> std::same_as<bool>;
    { handler.OnEnd() } -> std::same_as<void>;
};
*/

struct TextIteratorHandler {
    virtual bool OnColorCode(const CharEvent& e, const spring::u8string& text) { return true; }
    virtual bool OnColorCodeEx(const CharEvent& e, const spring::u8string& text) { return true; }
    virtual bool OnColorReset(const CharEvent& e) { return true; }
    virtual bool OnNewline(const CharEvent& e) { return true; }
    virtual bool OnPrintable(const CharEvent& e) { return true; }
    virtual bool OnSpace(const CharEvent& e) { return true; }
    virtual void OnEnd() {}
};

class TextIterator {
private:
    const spring::u8string& text;
    TextIteratorHandler& handler;
    int currentPos = 0;
    int skippedLines = 0;

public:
    TextIterator(const spring::u8string& text, TextIteratorHandler& handler)
        : text(text)
        , handler(handler)
    {}

    void Execute();
};