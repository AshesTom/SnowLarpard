// SPDX-License-Identifier: LGPL-2.1-or-later
#include "programmer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace Calculator {

namespace {

int precedence(int op)
{
    switch (op) {
    case PTimes:
    case PDivide: return 5;
    case PPlus:
    case PMinus: return 4;
    case ShiftLeftY:
    case ShiftRightY: return 3;
    case PAnd: return 2;
    case PXor: return 1;
    default: return 0;   // OR, NOR
    }
}

bool apply(int op, uint64_t a, uint64_t b, uint64_t &out)
{
    switch (op) {
    case PPlus: out = a + b; return true;
    case PMinus: out = a - b; return true;
    case PTimes: out = a * b; return true;
    case PDivide:
        if (b == 0)
            return false;
        out = a / b;
        return true;
    case PAnd: out = a & b; return true;
    case POr: out = a | b; return true;
    case PNor: out = ~(a | b); return true;
    case PXor: out = a ^ b; return true;
    case ShiftLeftY: out = b >= 64 ? 0 : a << b; return true;
    case ShiftRightY: out = b >= 64 ? 0 : a >> b; return true;
    default: out = b; return true;
    }
}

std::string inBase(uint64_t v, int base)
{
    if (v == 0)
        return "0";
    std::string s;
    while (v) {
        const int d = int(v % uint64_t(base));
        s += char(d < 10 ? '0' + d : 'A' + d - 10);
        v /= uint64_t(base);
    }
    std::reverse(s.begin(), s.end());
    return s;
}

// -[HexadecimalController okToProceed]: what a 64-bit number can hold in the base
size_t maxDigits(int base) { return base == 16 ? 16 : base == 8 ? 22 : 20; }

} // namespace

uint64_t Programmer::typed() const
{
    return m_input.empty() ? 0 : std::strtoull(m_input.c_str(), nullptr, base);
}

void Programmer::show(uint64_t v)
{
    m_value = v;
    m_error = false;
}

void Programmer::digit(const std::string &d)
{
    // -[HexadecimalController handleDigit:]; in RPN a result (state 2 or 5) goes onto the stack first
    if (rpn) {
        if (m_state == AfterOperator || m_state == AfterShift)
            m_stack.insert(m_stack.begin(), m_value);
        if (m_state != Typing)
            m_input.clear();
        m_equation.clear();
        m_state = Typing;
    }
    if (m_state != Typing && m_state != AfterOperator)
        m_equation.clear();
    if (m_state != Typing)
        m_input.clear();
    for (char c : d) {
        if (m_input.size() >= maxDigits(base))
            break;
        if (m_input == "0")
            m_input.clear();
        m_input += char(std::toupper(static_cast<unsigned char>(c)));
    }
    if (base == 10 && m_input.size() == 20 && m_input > "18446744073709551615")
        m_input.pop_back();
    m_state = Typing;
    show(typed());
}

void Programmer::setBase(int b)
{
    base = b;
    if (m_state == Typing)
        m_input = inBase(m_value, base);
}

void Programmer::deleteDigit()
{
    if (m_state != Typing || m_input.empty())
        return;
    m_input.pop_back();
    show(typed());
}

void Programmer::op(int key)
{
    if (rpn)
        return rpnOp(key);
    // -[HexadecimalController handleOperatorPress:]
    const bool shift1 = key == ShiftLeft1 || key == ShiftRight1;
    auto shifted = [key](uint64_t v) { return key == ShiftLeft1 ? v << 1 : v >> 1; };
    switch (m_state) {
    case Initial:
    case Typing: {
        const uint64_t n = typed();
        m_lastEntry = n;
        m_equation.push_back({true, shift1 ? shifted(n) : n, 0});
        break;
    }
    case AfterOperator:
        // another operator takes the place of the last
        if (!m_equation.empty() && !m_equation.back().isNumber)
            m_equation.pop_back();
        if (shift1 && !m_equation.empty() && m_equation.back().isNumber)
            m_equation.back().number = shifted(m_equation.back().number);
        break;
    case AfterEnter:   // RPN's only
    case AfterEquals:
    case AfterShift:
        m_equation = {{true, shift1 ? shifted(m_value) : m_value, 0}};
        break;
    }
    uint64_t v = 0;
    if (shift1) {
        // (n<<1): the equation evaluated at once
        if (evaluate(m_equation, v))
            show(v);
        m_state = AfterShift;
        return;
    }
    // + and − show what the equation comes to so far
    if ((key == PPlus || key == PMinus) && evaluate(m_equation, v))
        show(v);
    m_equation.push_back({false, 0, key});
    m_lastOp = key;
    m_state = AfterOperator;
}

void Programmer::equals()
{
    // -[HexadecimalController equals:]
    switch (m_state) {
    case Typing:
    case AfterOperator:
        m_lastEntry = typed();
        m_equation.push_back({true, m_lastEntry, 0});
        break;
    case AfterEquals:
        if (m_lastOp) {
            m_equation.push_back({false, 0, m_lastOp});
            m_equation.push_back({true, m_lastEntry, 0});
        }
        break;
    default:
        m_equation = {{true, m_state == AfterShift ? m_value : typed(), 0}};
        break;
    }
    m_state = AfterEquals;
    uint64_t v = 0;
    if (evaluate(m_equation, v))
        show(v);
    else
        m_error = true;
}

void Programmer::bits(int key)
{
    if (rpn)
        return rpnOp(key);
    // -[HexadecimalController handleBitManipulation:] (0x842d)
    uint64_t x = m_state == Typing ? typed() : m_state == Initial ? 0 : m_value;
    uint64_t r = 0;
    switch (key) {
    case OnesComplement: r = ~x; break;
    case TwosComplement: r = ~x + 1; break;
    case ByteFlip:
        for (; x; x >>= 8)
            r = r << 8 | (x & 0xff);
        break;
    case WordFlip:
        for (; x; x >>= 16)
            r = r << 16 | (x & 0xffff);
        break;
    case RotateLeft: r = x ? x << 1 | (x >> 63) : 0; break;
    case RotateRight: r = x << 63 | x >> 1; break;
    default: break;
    }
    show(r);
    m_equation.clear();
    m_input = inBase(r, base);
    m_state = Typing;
}

void Programmer::setBit(int bit, bool on)
{
    // -[HexadecimalController binaryButtonPress:]: the number shown, that bit changed
    uint64_t v = m_value;
    if (on)
        v |= uint64_t(1) << bit;
    else
        v &= ~(uint64_t(1) << bit);
    if (m_state != Typing && m_state != AfterOperator)
        m_equation.clear();
    show(v);
    m_input = inBase(v, base);
    m_state = Typing;
}

void Programmer::clear()
{
    m_input.clear();
    m_state = m_state == Initial ? Initial : Typing;
    show(0);
}

void Programmer::allClear()
{
    m_input.clear();
    m_stack.clear();
    m_equation.clear();
    m_lastEntry = 0;
    m_lastOp = 0;
    m_state = Initial;
    show(0);
}

bool Programmer::evaluate(const std::vector<Item> &eq, uint64_t &out) const
{
    // operators by precedence, left to right (a trailing operator is left out)
    std::vector<uint64_t> values;
    std::vector<int> ops;
    auto reduce = [&]() {
        const uint64_t b = values.back();
        values.pop_back();
        const uint64_t a = values.back();
        values.pop_back();
        uint64_t r = 0;
        const bool ok = apply(ops.back(), a, b, r);
        ops.pop_back();
        values.push_back(r);
        return ok;
    };
    for (size_t i = 0; i < eq.size(); ++i) {
        const Item &it = eq[i];
        if (it.isNumber) {
            values.push_back(it.number);
            continue;
        }
        if (i + 1 == eq.size())
            break;
        while (!ops.empty() && precedence(ops.back()) >= precedence(it.op))
            if (!reduce())
                return false;
        ops.push_back(it.op);
    }
    while (!ops.empty() && values.size() >= 2)
        if (!reduce())
            return false;
    out = values.empty() ? 0 : values.back();
    return true;
}

std::string Programmer::rowString(uint64_t v) const
{
    // -[LCDController tableView:objectValueForTableColumn:row:]: "0x%llX", "%llo", or decimal
    return (base == 16 ? "0x" : "") + Calculator::inBase(v, base);
}

void Programmer::setRPN(bool on)
{
    // -setRPNMode:: the number shown kept, the equation dropped
    rpn = on;
    const uint64_t v = m_state == Typing ? typed() : m_value;
    m_equation.clear();
    m_stack.clear();
    m_input.clear();
    m_state = Initial;
    show(v);
}

void Programmer::enter()
{
    // -[HexadecimalController enter:]: the number onto the stack (not at the start), x kept
    if (m_state == Typing)
        show(typed());
    if (m_state != Initial || m_value != 0)
        m_stack.insert(m_stack.begin(), m_value);
    m_input.clear();
    m_state = AfterEnter;
}

void Programmer::rpnDrop()
{
    // -rpnDrop: y comes to x, or 0
    if (!m_stack.empty()) {
        show(m_stack.front());
        m_stack.erase(m_stack.begin());
    } else {
        show(0);
    }
    m_input.clear();
    m_state = Initial;
}

void Programmer::rpnExchange()
{
    if (m_stack.empty())
        return;
    const uint64_t x = m_state == Typing ? typed() : m_value;
    show(m_stack.front());
    m_stack.front() = x;
    m_input.clear();
    m_state = Initial;
}

void Programmer::rpnRoll(bool up)
{
    // -rollStack:: x with the stack turned one way or the other
    const uint64_t x = m_state == Typing ? typed() : m_value;
    m_stack.insert(m_stack.begin(), x);
    if (up)
        std::rotate(m_stack.begin(), m_stack.end() - 1, m_stack.end());
    else
        std::rotate(m_stack.begin(), m_stack.begin() + 1, m_stack.end());
    show(m_stack.front());
    m_stack.erase(m_stack.begin());
    m_input.clear();
    m_state = Initial;
}

void Programmer::rpnOp(int key)
{
    // -[HexadecimalController performRPNOperation:withX:] (0x53f9): y from the stack for the
    // binary ones (a beep without), the result shown (state 2); ÷ by 0: "Division by zero", y back
    const uint64_t x = m_state == Typing ? typed() : m_value;
    const bool unary = key == TwosComplement || key == ShiftLeft1 || key == ShiftRight1 || key == ByteFlip || key == WordFlip ||
                       key == OnesComplement || key == RotateLeft || key == RotateRight;
    uint64_t y = 0;
    if (!unary) {
        if (m_stack.empty()) {
            std::fputc('\a', stderr);
            return;
        }
        y = m_stack.front();
        m_stack.erase(m_stack.begin());
    }
    uint64_t r = x;
    switch (key) {
    case PPlus: r = x + y; break;
    case PMinus: r = y - x; break;
    case PTimes: r = x * y; break;
    case PDivide:
        if (x == 0) {
            m_stack.insert(m_stack.begin(), y);
            m_error = true;
            m_input.clear();
            m_state = AfterOperator;
            return;
        }
        r = y / x;
        break;
    case TwosComplement: r = uint64_t(0) - x; break;
    case PAnd: r = x & y; break;
    case POr: r = x | y; break;
    case PNor: r = ~(x | y); break;
    case PXor: r = x ^ y; break;
    case ShiftLeft1: r = x * 2; break;
    case ShiftRight1: r = x >> 1; break;
    case ShiftLeftY: r = y << (x & 0x3f); break;
    case ShiftRightY: r = y >> (x & 0x3f); break;
    case ByteFlip:
        r = 0;
        for (uint64_t v = x; v; v >>= 8)
            r = r << 8 | (v & 0xff);
        break;
    case WordFlip:
        r = 0;
        for (uint64_t v = x; v; v >>= 16)
            r = r << 16 | (v & 0xffff);
        break;
    case OnesComplement: r = ~x; break;
    case RotateLeft: r = x << 1 | (x >> 63); break;
    case RotateRight: r = x << 63 | x >> 1; break;
    default: break;
    }
    show(r);
    m_input.clear();
    m_state = AfterOperator;
}

std::string Programmer::display() const
{
    if (m_state == Typing && !m_input.empty()) {
        return (base == 16 ? "0x" : "") + m_input;
    }
    return (base == 16 ? "0x" : "") + inBase(m_value, base);
}

} // namespace Calculator
