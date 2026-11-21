// SPDX-License-Identifier: LGPL-2.1-or-later
// Calculator's Programmer view (Hexadecimal.calcview's HexadecimalController, 10.6), algebraic: the
// keys build an equation of unsigned 64-bit numbers and operators that the Calculate framework
// evaluates (-[CalculatorController sendDataToCalcEngine:]); + and − show what the equation comes
// to so far, the other operators wait; << and >> shift the number by one at once, = evaluates and,
// pressed again, repeats the last operator with the last number. The bit keys (1's, 2's, RoL, RoR,
// Byte Flip, Word Flip) change the number shown (-[HexadecimalController handleBitManipulation:]).
// Calculate's precedences are C's here (× ÷, + −, << >>, AND, XOR, OR NOR): the framework isn't read.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Calculator {

// The nib's tags
enum ProgrammerKey {
    PPlus = 100, PMinus = 101, PTimes = 102, PDivide = 103, TwosComplement = 104, PAnd = 105, POr = 106, PNor = 107,
    PXor = 108, ShiftLeft1 = 109, ShiftRight1 = 110, ShiftLeftY = 111, ShiftRightY = 112, ByteFlip = 113,
    WordFlip = 114, PClear = 115, PAllClear = 116, OnesComplement = 117, RotateLeft = 118, RotateRight = 119,
    PEquals = 120
};

class Programmer {
public:
    int base = 10;   // 8, 10 or 16 (the 8 / 10 / 16 control's tags)
    void setBase(int b);   // the number being typed written again in it

    void digit(const std::string &d);   // "0"... "F", "00", "FF"
    void op(int key);
    void equals();
    void bits(int key);
    void clear();      // C: the number being typed
    void allClear();   // AC
    void setBit(int bit, bool on);   // the binary view's buttons
    void deleteDigit();

    // RPN (-[HexadecimalController setRPNMode:]: = becomes enter): enter puts the number on the
    // stack; a number typed after a result puts the result on it first; the operators take y from
    // it (performRPNOperation:withX:), the bit keys work on x; drop, x↔y, R↑ and R↓
    bool rpn = false;
    void setRPN(bool on);
    void enter();
    void rpnDrop();
    void rpnExchange();
    void rpnRoll(bool up);
    const std::vector<uint64_t> &stack() const { return m_stack; }   // its top (y) first
    std::string rowString(uint64_t v) const;   // as the LCD's stack rows write it

    uint64_t value() const { return m_value; }
    std::string display() const;   // in the base: 0x... in hexadecimal
    bool error() const { return m_error; }

private:
    struct Item {
        bool isNumber;
        uint64_t number;
        int op;
    };
    uint64_t typed() const;
    bool evaluate(const std::vector<Item> &eq, uint64_t &out) const;
    void show(uint64_t v);

    void rpnOp(int key);   // performRPNOperation:withX:
    enum State { Initial = 0, Typing = 1, AfterOperator = 2, AfterEnter = 3, AfterEquals = 4, AfterShift = 5 } m_state = Initial;
    std::vector<uint64_t> m_stack;
    std::string m_input;   // the digits typed, in the base
    std::vector<Item> m_equation;
    uint64_t m_value = 0;
    uint64_t m_lastEntry = 0;
    int m_lastOp = 0;
    bool m_error = false;
};

} // namespace Calculator
