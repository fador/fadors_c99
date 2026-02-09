int main() {
    // 1. Multiplicative vs Additive
    // 1 + 2 * 3 should be 1 + (2 * 3) = 7
    if (1 + 2 * 3 != 7) return 1;
    
    // 2. Additive vs Shift
    // 1 << 2 + 1 should be 1 << (2 + 1) = 8
    if (1 << 2 + 1 != 8) return 2;
    
    // 3. Shift vs Relational
    // 5 < 1 << 3 should be 5 < (1 << 3) = 5 < 8 = 1
    if (!(5 < 1 << 3)) return 3;
    
    // 4. Relational vs Equality
    // 1 == 2 < 3 should be 1 == (2 < 3) = 1 == 1 = 1
    if (!(1 == 2 < 3)) return 4;
    
    // 5. Equality vs Bitwise AND
    // 5 & 3 == 3 should be 5 & (3 == 3) = 5 & 1 = 1
    if ((5 & 3 == 3) != 1) return 5;
    
    // 6. Bitwise AND vs XOR vs OR
    // 7 ^ 3 | 4 & 2 should be (7 ^ 3) | (4 & 2) 
    // Wait, precedence is & > ^ > |
    // So 7 ^ 3 | 4 & 2 is 7 ^ (3 | (4 & 2))? No.
    // Precedence: & (8), ^ (9), | (10) [Higher number = lower precedence in my head, let's check parse order]
    // parse_inclusive_or (|) calls parse_exclusive_or (^) calls parse_and (&)
    // So it's 7 ^ 3 | (4 & 2) -> (7 ^ 3) | (4 & 2) ? 
    // No, parse_inclusive_or will have a node where left is parse_exclusive_or and right is parse_exclusive_or.
    // So (7 ^ 3) | (4 & 2) is correct.
    // 7 ^ 3 = 111 ^ 011 = 100 (4)
    // 4 & 2 = 100 & 010 = 000 (0)
    // 4 | 0 = 4
    if ((7 ^ 3 | 4 & 2) != 4) return 6;
    
    // 7. Logical AND vs Logical OR
    // 1 || 0 && 0 should be 1 || (0 && 0) = 1 || 0 = 1
    if (!(1 || 0 && 0)) return 7;
    
    // 8. Unary vs Multiplicative
    // -2 * 3 should be (-2) * 3 = -6
    if (-2 * 3 != -6) return 8;
    
    // 9. Complex mixed
    // !0 && 5 + 2 << 1 == 14 should be:
    // (!0) && (((5 + 2) << 1) == 14)
    // 1 && ((7 << 1) == 14)
    // 1 && (14 == 14)
    // 1 && 1 = 1
    if (!(!0 && 5 + 2 << 1 == 14)) return 9;

    return 42;
}
