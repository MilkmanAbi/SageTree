gc_disable()
# Regular expression engine
# Supports: literal, ., *, +, ?, [], [^], ^, $, |, (), \d, \w, \s, escapes

# ============================================================================
# Pattern compilation (regex string -> instruction list)
# ============================================================================

let OP_LITERAL = 1
let OP_DOT = 2
let OP_STAR = 3
let OP_PLUS = 4
let OP_QUESTION = 5
let OP_CHAR_CLASS = 6
let OP_NEG_CLASS = 7
let OP_ANCHOR_START = 8
let OP_ANCHOR_END = 9
let OP_GROUP_START = 10
let OP_GROUP_END = 11
let OP_ALT = 12
let OP_DIGIT = 13
let OP_WORD = 14
let OP_SPACE = 15
let OP_NOT_DIGIT = 16
let OP_NOT_WORD = 17
let OP_NOT_SPACE = 18

proc is_digit(c):
    let code = ord(c)
    return code >= 48 and code <= 57

proc is_word_char(c):
    let code = ord(c)
    if code >= 48 and code <= 57:
        return true
    if code >= 65 and code <= 90:
        return true
    if code >= 97 and code <= 122:
        return true
    if code == 95:
        return true
    return false

proc is_space(c):
    let code = ord(c)
    return code == 32 or code == 9 or code == 10 or code == 13

# Compile a regex pattern string into an instruction list
proc compile(pattern):
    let ops = []
    var i = 0
    while i < pattern.length:
        let c = pattern[i]
        if c == ".":
            ops.push({"op": 2})
            i = i + 1
            continue
        if c == "^":
            ops.push({"op": 8})
            i = i + 1
            continue
        if c == "$":
            ops.push({"op": 9})
            i = i + 1
            continue
        if c == chr(92) and i + 1 < pattern.length:
            let nc = pattern[i + 1]
            if nc == "d":
                ops.push({"op": 13})
            if nc == "D":
                ops.push({"op": 16})
            if nc == "w":
                ops.push({"op": 14})
            if nc == "W":
                ops.push({"op": 17})
            if nc == "s":
                ops.push({"op": 15})
            if nc == "S":
                ops.push({"op": 18})
            if nc != "d" and nc != "D" and nc != "w" and nc != "W" and nc != "s" and nc != "S":
                ops.push({"op": 1, "ch": nc})
            i = i + 2
            continue
        if c == "[":
            var chars = ""
            var neg = false
            i = i + 1
            if i < pattern.length and pattern[i] == "^":
                neg = true
                i = i + 1
            while i < pattern.length and pattern[i] != "]":
                if pattern[i] == "-" and chars.length > 0 and i + 1 < pattern.length and pattern[i + 1] != "]":
                    let from_code = ord(chars[chars.length - 1])
                    let to_code = ord(pattern[i + 1])
                    var j = from_code + 1
                    while j <= to_code:
                        chars = chars + chr(j)
                        j = j + 1
                    i = i + 2
                else:
                    chars = chars + pattern[i]
                    i = i + 1
            i = i + 1
            if neg:
                ops.push({"op": 7, "chars": chars})
            else:
                ops.push({"op": 6, "chars": chars})
            continue
        # Quantifiers modify the previous op
        if c == "*":
            if ops.length > 0:
                let prev = ops[ops.length - 1]
                let wrapped = {"op": 3, "inner": prev}
                ops[ops.length - 1] = wrapped
            i = i + 1
            continue
        if c == "+":
            if ops.length > 0:
                let prev = ops[ops.length - 1]
                let wrapped = {"op": 4, "inner": prev}
                ops[ops.length - 1] = wrapped
            i = i + 1
            continue
        if c == "?":
            if ops.length > 0:
                let prev = ops[ops.length - 1]
                let wrapped = {"op": 5, "inner": prev}
                ops[ops.length - 1] = wrapped
            i = i + 1
            continue
        # Default: literal character
        ops.push({"op": 1, "ch": c})
        i = i + 1
    return ops

# ============================================================================
# Matching engine (backtracking NFA)
# ============================================================================

proc match_op(op, text, pos):
    if pos >= text.length:
        return -1
    let c = text[pos]
    let opcode = op["op"]
    if opcode == 1:
        if c == op["ch"]:
            return pos + 1
        return -1
    if opcode == 2:
        return pos + 1
    if opcode == 13:
        if is_digit(c):
            return pos + 1
        return -1
    if opcode == 16:
        if not is_digit(c):
            return pos + 1
        return -1
    if opcode == 14:
        if is_word_char(c):
            return pos + 1
        return -1
    if opcode == 17:
        if not is_word_char(c):
            return pos + 1
        return -1
    if opcode == 15:
        if is_space(c):
            return pos + 1
        return -1
    if opcode == 18:
        if not is_space(c):
            return pos + 1
        return -1
    if opcode == 6:
        var chars = op["chars"]
        var found = false
        for j in 0..chars.length:
            if not found and c == chars[j]:
                found = true
        if found:
            return pos + 1
        return -1
    if opcode == 7:
        var chars = op["chars"]
        var found = false
        for j in 0..chars.length:
            if not found and c == chars[j]:
                found = true
        if not found:
            return pos + 1
        return -1
    return -1

proc match_ops(ops, idx, text, pos):
    if idx >= ops.length:
        return pos
    let op = ops[idx]
    let opcode = op["op"]
    # Anchor start
    if opcode == 8:
        if pos == 0:
            return match_ops(ops, idx + 1, text, pos)
        return -1
    # Anchor end
    if opcode == 9:
        if pos == text.length:
            return match_ops(ops, idx + 1, text, pos)
        return -1
    # Star (greedy)
    if opcode == 3:
        let inner = op["inner"]
        # Try matching as many as possible, then backtrack
        let positions = [pos]
        var p = pos
        let max_iter = text.length + 1
        var iter_count = 0
        while iter_count < max_iter:
            let np = match_op(inner, text, p)
            if np < 0:
                iter_count = max_iter
            else:
                positions.push(np)
                p = np
                iter_count = iter_count + 1
        # Try from longest match down
        var pi = positions.length - 1
        while pi >= 0:
            var result = match_ops(ops, idx + 1, text, positions[pi])
            if result >= 0:
                return result
            pi = pi - 1
        return -1
    # Plus (one or more, greedy)
    if opcode == 4:
        let inner = op["inner"]
        let first = match_op(inner, text, pos)
        if first < 0:
            return -1
        let positions = [first]
        var p = first
        let max_iter = text.length + 1
        var iter_count = 0
        while iter_count < max_iter:
            let np = match_op(inner, text, p)
            if np < 0:
                iter_count = max_iter
            else:
                positions.push(np)
                p = np
                iter_count = iter_count + 1
        var pi = positions.length - 1
        while pi >= 0:
            var result = match_ops(ops, idx + 1, text, positions[pi])
            if result >= 0:
                return result
            pi = pi - 1
        return -1
    # Question (zero or one)
    if opcode == 5:
        let inner = op["inner"]
        let with_match = match_op(inner, text, pos)
        if with_match >= 0:
            var result = match_ops(ops, idx + 1, text, with_match)
            if result >= 0:
                return result
        return match_ops(ops, idx + 1, text, pos)
    # Simple ops
    let next_pos = match_op(op, text, pos)
    if next_pos >= 0:
        return match_ops(ops, idx + 1, text, next_pos)
    return -1

# ============================================================================
# Public API
# ============================================================================

# Test if pattern matches anywhere in text
proc search(pattern, text):
    let ops = compile(pattern)
    for i in 0..text.length:
        var result = match_ops(ops, 0, text, i)
        if result >= 0:
            let m = {}
            m["start"] = i
            m["end"] = result
            m["text"] = ""
            for j in 0..result - i:
                m["text"] = m["text"] + text[i + j]
            return m
    return nil

# Test if pattern matches the entire text
proc full_match(pattern, text):
    let ops = compile(pattern)
    var result = match_ops(ops, 0, text, 0)
    if result == text.length:
        return true
    return false

# Test if pattern matches (returns boolean)
proc test(pattern, text):
    return search(pattern, text) != nil

# Find all non-overlapping matches
proc find_all(pattern, text):
    let ops = compile(pattern)
    let results = []
    var i = 0
    while i < text.length:
        var result = match_ops(ops, 0, text, i)
        if result >= 0 and result > i:
            let m = {}
            m["start"] = i
            m["end"] = result
            m["text"] = ""
            for j in 0..result - i:
                m["text"] = m["text"] + text[i + j]
            results.push(m)
            i = result
        else:
            i = i + 1
    return results

# Replace first match
proc replace_first(pattern, text, replacement):
    let m = search(pattern, text)
    if m == nil:
        return text
    var result = ""
    for i in 0..m["start"]:
        result = result + text[i]
    result = result + replacement
    for i in 0..text.length - m["end"]:
        result = result + text[m["end"] + i]
    return result

# Replace all matches
proc replace_all(pattern, text, replacement):
    let ops = compile(pattern)
    var result = ""
    var i = 0
    while i < text.length:
        let end_pos = match_ops(ops, 0, text, i)
        if end_pos >= 0 and end_pos > i:
            result = result + replacement
            i = end_pos
        else:
            result = result + text[i]
            i = i + 1
    return result

# Split text by pattern
proc split_by(pattern, text):
    let ops = compile(pattern)
    let parts = []
    var current = ""
    var i = 0
    while i < text.length:
        let end_pos = match_ops(ops, 0, text, i)
        if end_pos >= 0 and end_pos > i:
            parts.push(current)
            current = ""
            i = end_pos
        else:
            current = current + text[i]
            i = i + 1
    parts.push(current)
    return parts
